// Copyright 2024 The ChromiumOS Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use std::num::TryFromIntError;
use std::rc::Rc;
use std::sync::Arc;

use libva::AV1EncLoopFilterFlags;
use libva::AV1EncLoopRestorationFlags;
use libva::AV1EncModeControlFlags;
use libva::AV1EncPictureFlags;
use libva::AV1EncQMatrixFlags;
use libva::AV1EncSeqFields;
use libva::AV1EncTileGroupObuHdrInfo;
use libva::BufferType;
use libva::EncCodedBuffer;
use libva::EncPackedHeaderParameter;
use libva::EncPackedHeaderType;
use libva::EncPictureParameterBufferAV1;
use libva::EncSegParamAV1;
use libva::EncSegParamFlagsAV1;
use libva::EncSequenceParameterBufferAV1;
use libva::EncTileGroupBufferAV1;
use libva::Picture;
use libva::RefFrameCtrlAV1;
use libva::Surface;
use libva::SurfaceMemoryDescriptor;
use libva::VAProfile::VAProfileAV1Profile0;
use libva::VAProfile::VAProfileAV1Profile1;
use libva::VaError;
use libva::VA_INVALID_ID;

use crate::backend::vaapi::encoder::tunings_to_libva_rc;
use crate::backend::vaapi::encoder::CodedOutputPromise;
use crate::backend::vaapi::encoder::Reconstructed;
use crate::backend::vaapi::encoder::VaapiBackend;
use crate::codec::av1::parser::FrameHeaderObu;
use crate::codec::av1::parser::FrameType;
use crate::codec::av1::parser::Profile;
use crate::codec::av1::parser::ReferenceFrameType;
use crate::codec::av1::parser::SequenceHeaderObu;
use crate::codec::av1::parser::TemporalDelimiterObu;
use crate::codec::av1::parser::CDEF_MAX;
use crate::codec::av1::parser::MAX_SEGMENTS;
use crate::codec::av1::parser::MAX_TILE_COLS;
use crate::codec::av1::parser::MAX_TILE_ROWS;
use crate::codec::av1::parser::REFS_PER_FRAME;
use crate::codec::av1::parser::SEG_LVL_MAX;
use crate::codec::av1::synthesizer::FrameHeaderBitOffsets;
use crate::codec::av1::synthesizer::Synthesizer;
use crate::encoder::av1::EncoderConfig;
use crate::encoder::av1::AV1;
use crate::encoder::stateless::av1::predictor::EncoderFeaturesAV1;
use crate::encoder::stateless::av1::predictor::MAX_BASE_QINDEX;
use crate::encoder::stateless::av1::predictor::MIN_BASE_QINDEX;
use crate::encoder::stateless::av1::BackendRequest;
use crate::encoder::stateless::av1::StatelessAV1EncoderBackend;
use crate::encoder::stateless::ReadyPromise;
use crate::encoder::stateless::StatelessBackendError;
use crate::encoder::stateless::StatelessBackendResult;
use crate::encoder::stateless::StatelessEncoder;
use crate::encoder::stateless::StatelessVideoEncoderBackend;
use crate::encoder::EncodeResult;
use crate::encoder::RateControl;
use crate::video_frame::VideoFrame;
use crate::BlockingMode;
use crate::Fourcc;
use crate::Resolution;

type Request<H> = BackendRequest<H, Reconstructed>;

#[derive(thiserror::Error, Debug)]
pub enum BackendError {
    #[error(transparent)]
    ConversionError(#[from] TryFromIntError),

    #[error("vaBeginPicture failed: {0}")]
    BeginPictureError(VaError),
    #[error("vaRenderPicture failed: {0}")]
    RenderPictureError(VaError),
    #[error("vaRenderPicture failed: {0}")]
    EndPictureError(VaError),
    #[error(transparent)]
    Other(#[from] anyhow::Error),
}

impl From<BackendError> for StatelessBackendError {
    fn from(value: BackendError) -> Self {
        StatelessBackendError::Other(anyhow::anyhow!(value))
    }
}

type Result<T> = std::result::Result<T, BackendError>;

/// Parse AV1 encoder features from the raw VAConfigAttribValEncAV1 u32 value.
///
/// Bit layout (each feature occupies 2 bits):
///   [1:0]   128x128_superblock   [3:2]   filter_intra
///   [5:4]   intra_edge_filter    [7:6]   interintra_compound
///   [9:8]   masked_compound      [11:10] warped_motion
///   [13:12] palette_mode         [15:14] dual_filter
///   [17:16] jnt_comp             [19:18] ref_frame_mvs
///   [21:20] superres             [23:22] restoration
///   [25:24] allow_intrabc        [27:26] cdef_channel_strength
fn parse_av1_enc_features(value: u32) -> EncoderFeaturesAV1 {
    let bit = |shift: u32| -> bool { ((value >> shift) & 0x3) != 0 };

    EncoderFeaturesAV1 {
        support_128x128_superblock: bit(0),
        support_filter_intra: bit(2),
        support_intra_edge_filter: bit(4),
        support_interintra_compound: bit(6),
        support_masked_compound: bit(8),
        support_warped_motion: bit(10),
        support_dual_filter: bit(14),
        support_jnt_comp: bit(16),
        support_ref_frame_mvs: bit(18),
        support_superres: bit(20),
        support_restoration: bit(22),
        ..Default::default()
    }
}

/// Parse VAConfigAttribValEncAV1Ext2 fields into an existing features struct.
fn apply_av1_enc_ext2(features: &mut EncoderFeaturesAV1, value: u32) {
    features.tx_mode_support = ((value >> 4) & 0x7) as u8;
    features.max_tile_num_minus1 = ((value >> 7) & 0x1fff) as u16;
}

impl<M, Handle> StatelessVideoEncoderBackend<AV1> for VaapiBackend<M, Handle>
where
    M: SurfaceMemoryDescriptor,
    Handle: std::borrow::Borrow<Surface<M>>,
{
    type Picture = Handle;
    type Reconstructed = Reconstructed;
    type CodedPromise = CodedOutputPromise<M, Handle>;
    type ReconPromise = ReadyPromise<Self::Reconstructed>;
}

impl<M, H> VaapiBackend<M, H>
where
    M: SurfaceMemoryDescriptor,
    H: std::borrow::Borrow<Surface<M>> + 'static,
{
    /// Query AV1 encoder capabilities from the driver using
    /// VAConfigAttribEncAV1 and VAConfigAttribEncAV1Ext2.
    fn query_av1_enc_features(&self) -> StatelessBackendResult<EncoderFeaturesAV1> {
        let display = self.context().display();

        // Query VAConfigAttribEncAV1 (features)
        let mut attrs = [libva::VAConfigAttrib {
            type_: libva::VAConfigAttribType::VAConfigAttribEncAV1,
            value: 0,
        }];

        display.get_config_attributes(self.va_profile, self.entrypoint, &mut attrs)?;

        let mut features = if attrs[0].value == libva::VA_ATTRIB_NOT_SUPPORTED {
            log::warn!(
                "Driver does not advertise AV1 encoder features, using conservative defaults"
            );
            EncoderFeaturesAV1::default()
        } else {
            log::info!("AV1 encoder features: {:#010x}", attrs[0].value);
            parse_av1_enc_features(attrs[0].value)
        };

        // Query VAConfigAttribEncAV1Ext2 (tx_mode, tile_size_bytes, etc.)
        let mut attrs = [libva::VAConfigAttrib {
            type_: libva::VAConfigAttribType::VAConfigAttribEncAV1Ext2,
            value: 0,
        }];

        display.get_config_attributes(self.va_profile, self.entrypoint, &mut attrs)?;

        if attrs[0].value == libva::VA_ATTRIB_NOT_SUPPORTED {
            log::warn!("Driver does not advertise AV1 encoder ext2 attributes, using defaults");
        } else {
            log::info!("AV1 encoder ext2: {:#010x}", attrs[0].value);
            apply_av1_enc_ext2(&mut features, attrs[0].value);
        }

        log::info!("AV1 encoder features: {:?}", features);
        Ok(features)
    }

    fn build_seq_param(
        request: &Request<H>,
        enable_cdef: bool,
    ) -> Result<EncSequenceParameterBufferAV1> {
        assert!(
            request.sequence.operating_points_cnt_minus_1 == 0,
            "Only a single operating point is supported for now"
        );
        const OPERATING_POINT: usize = 0;

        let seq_profile = request.sequence.seq_profile as u8;
        let seq_level_idx = request.sequence.operating_points[OPERATING_POINT].seq_level_idx;
        let seq_tier = request.sequence.operating_points[OPERATING_POINT].seq_tier;
        let hierarchical_flag = 0;

        let bits_per_second = request.tunings.rate_control.bitrate_maximum().unwrap_or(0) as u32;

        // AV1 5.5.2
        let bit_depth_minus8 = if request.sequence.seq_profile == Profile::Profile2
            && request.sequence.color_config.high_bitdepth
        {
            if request.sequence.color_config.twelve_bit {
                12
            } else {
                10
            }
        } else if request.sequence.color_config.high_bitdepth {
            10
        } else {
            8
        };

        let order_hint_bits_minus_1 = u8::try_from(request.sequence.order_hint_bits_minus_1)?;

        Ok(EncSequenceParameterBufferAV1::new(
            seq_profile,
            seq_level_idx,
            seq_tier,
            hierarchical_flag,
            request.intra_period,
            request.ip_period,
            bits_per_second,
            &AV1EncSeqFields::new(
                request.sequence.still_picture,
                request.sequence.use_128x128_superblock,
                request.sequence.enable_filter_intra,
                request.sequence.enable_intra_edge_filter,
                request.sequence.enable_interintra_compound,
                request.sequence.enable_masked_compound,
                request.sequence.enable_warped_motion,
                request.sequence.enable_dual_filter,
                request.sequence.enable_order_hint,
                request.sequence.enable_jnt_comp,
                request.sequence.enable_ref_frame_mvs,
                request.sequence.enable_superres,
                enable_cdef,
                request.sequence.enable_restoration,
                bit_depth_minus8,
                request.sequence.color_config.subsampling_x,
                request.sequence.color_config.subsampling_y,
                request.sequence.color_config.mono_chrome,
            ),
            order_hint_bits_minus_1,
        ))
    }

    fn build_ref_ctrl(
        refs: &[Option<Rc<Reconstructed>>; REFS_PER_FRAME],
        ctrl: &[ReferenceFrameType; REFS_PER_FRAME],
    ) -> RefFrameCtrlAV1 {
        let ctrl = ctrl.map(|type_| {
            if type_ == ReferenceFrameType::Intra {
                return 0;
            }

            let idx = type_ as u32 - ReferenceFrameType::Last as u32;
            if refs[idx as usize].is_none() {
                return 0;
            }

            type_ as u32
        });

        RefFrameCtrlAV1::new(ctrl[0], ctrl[1], ctrl[2], ctrl[3], ctrl[4], ctrl[5], ctrl[6])
    }

    fn build_pic_param(
        request: &Request<H>,
        recon: &Reconstructed,
        coded: &EncCodedBuffer,
        fh_offsets: &FrameHeaderBitOffsets,
        packed_seq_data_len: usize,
        use_packed_headers: bool,
    ) -> Result<EncPictureParameterBufferAV1> {
        let coded_buf = coded.id();
        let reconstructed_frame = recon.surface_id();

        let mut reference_frames = [VA_INVALID_ID; 8];

        for (i, frame) in reference_frames.iter_mut().enumerate().take(REFS_PER_FRAME) {
            let Some(ref_frame) = &request.references[i] else {
                continue;
            };

            *frame = ref_frame.surface_id();
        }

        let mut ref_frame_idx = [0; 7];
        for (i, idx) in ref_frame_idx.iter_mut().enumerate() {
            *idx = request.frame.ref_frame_idx[i];
        }

        let frame_width_minus_1 = u16::try_from(request.frame.frame_width - 1)?;
        let frame_height_minus_1 = u16::try_from(request.frame.frame_height - 1)?;

        // Single temporal layer is used.
        const HIERARCHICAL_LEVEL_PLUS1: u8 = 0;

        let primary_ref_frame = u8::try_from(request.frame.primary_ref_frame)?;
        let order_hint = u8::try_from(request.frame.order_hint)?;
        let refresh_frame_flags = u8::try_from(request.frame.refresh_frame_flags)?;

        let ref_frame_ctrl_l0 =
            Self::build_ref_ctrl(&request.references, &request.ref_frame_ctrl_l0);
        let ref_frame_ctrl_l1 =
            Self::build_ref_ctrl(&request.references, &request.ref_frame_ctrl_l1);

        let frame_type = request.frame.frame_type as u32;

        // Export Frame Obu rather then TileGroup Obu
        const ENABLE_FRAME_OBU: bool = false;

        // We don't use long term reference frames for now.
        const LONG_TERM_REFERENCE: bool = false;

        // Current we always expect the reconstructed frame.
        const DISABLE_FRAME_RECON: bool = false;

        // Palette mode is not used. This also implies force_integer_mv and
        // allow_screen_content_tools should be false.
        const PALETTE_MODE_ENABLE: bool = false;
        const FORCE_INTEGER_MV: bool = false;
        const ALLOW_SCREEN_CONTENT_TOOLS: bool = false;

        // Use 16x16 block size for now.
        // TODO: Use maximum available
        const SEG_ID_BLOCK_SIZE: u8 = 0;

        // Use single tile group;
        const NUM_TILE_GROUPS_MINUS1: u8 = 0;

        let filter_level = [
            request.frame.loop_filter_params.loop_filter_level[0],
            request.frame.loop_filter_params.loop_filter_level[1],
        ];

        let filter_level_u = request.frame.loop_filter_params.loop_filter_level[2];
        let filter_level_v = request.frame.loop_filter_params.loop_filter_level[3];

        // VAAPI expects 0 when superres is not used, even though the AV1 bitstream uses
        // SUPERRES_NUM (8).
        let superres_scale_denominator = if request.frame.use_superres {
            u8::try_from(request.frame.superres_denom)?
        } else {
            0
        };

        let interpolation_filter = request.frame.interpolation_filter as u8;

        let mut loop_filter_ref_deltas = [0; 8];
        for (i, delta) in loop_filter_ref_deltas.iter_mut().enumerate() {
            *delta = request.frame.loop_filter_params.loop_filter_ref_deltas[i];
        }

        let base_qindex = u8::try_from(request.frame.quantization_params.base_q_idx)?;
        let y_dc_delta_q = i8::try_from(request.frame.quantization_params.delta_q_y_dc)?;
        let u_dc_delta_q = i8::try_from(request.frame.quantization_params.delta_q_u_dc)?;
        let u_ac_delta_q = i8::try_from(request.frame.quantization_params.delta_q_u_ac)?;
        let v_dc_delta_q = i8::try_from(request.frame.quantization_params.delta_q_v_dc)?;
        let v_ac_delta_q = i8::try_from(request.frame.quantization_params.delta_q_v_ac)?;

        // Clamp quality range
        let min_base_qindex = request.tunings.min_quality.max(MIN_BASE_QINDEX);
        let min_base_qindex = u8::try_from(min_base_qindex)?;
        let max_base_qindex = request.tunings.max_quality.min(MAX_BASE_QINDEX);
        let max_base_qindex = u8::try_from(max_base_qindex)?;

        let qm_y = u16::try_from(request.frame.quantization_params.qm_y)?;
        let qm_u = u16::try_from(request.frame.quantization_params.qm_u)?;
        let qm_v = u16::try_from(request.frame.quantization_params.qm_v)?;

        let tx_mode = request.frame.tx_mode as u32;

        // Make driver make decision use single reference or compound reference.
        const REFERENCE_MODE: u32 = 0 /* REFERENCE_MODE_SELECT */;

        let segmentation_temporal_update =
            request.frame.segmentation_params.segmentation_temporal_update;

        const SEGMENT_NUMBER: u8 = 0;
        assert!(
            !request.frame.segmentation_params.segmentation_enabled,
            "Unsupported segmentation_enabled=1"
        );

        // Segementation feature mask
        let mut feature_mask = [0u8; MAX_SEGMENTS];
        for (seg, mask) in feature_mask.iter_mut().enumerate() {
            for lvl in 0..u8::try_from(SEG_LVL_MAX)? {
                if request.frame.segmentation_params.feature_enabled[seg][lvl as usize] {
                    *mask |= 1u8 << lvl;
                }
            }
        }

        assert!(
            request.frame.tile_info.tile_cols == 1
                && request.frame.tile_info.tile_cols_log2 == 0
                && request.frame.tile_info.tile_rows == 1
                && request.frame.tile_info.tile_rows_log2 == 0,
            "Single tile is only supported for now"
        );
        let tile_cols = u8::try_from(request.frame.tile_info.tile_cols)?;
        let tile_rows = u8::try_from(request.frame.tile_info.tile_rows)?;

        let mut width_in_sbs_minus_1 = [0u16; MAX_TILE_COLS - 1];
        for (i, width) in width_in_sbs_minus_1.iter_mut().enumerate() {
            *width = u16::try_from(request.frame.tile_info.width_in_sbs_minus_1[i])?;
        }

        let mut height_in_sbs_minus_1 = [0u16; MAX_TILE_ROWS - 1];
        for (i, height) in height_in_sbs_minus_1.iter_mut().enumerate() {
            *height = u16::try_from(request.frame.tile_info.height_in_sbs_minus_1[i])?;
        }

        let context_update_tile_id = u16::try_from(request.frame.tile_info.context_update_tile_id)?;

        let cdef_damping_minus_3 = u8::try_from(request.frame.cdef_params.cdef_damping - 3)?;

        let cdef_bits = u8::try_from(request.frame.cdef_params.cdef_bits)?;
        let mut cdef_y_strengths = [0u8; CDEF_MAX];
        for (i, strength) in cdef_y_strengths.iter_mut().enumerate() {
            *strength = u8::try_from(request.frame.cdef_params.cdef_y_pri_strength[i])?;
        }

        let mut cdef_uv_strengths = [0u8; CDEF_MAX];
        for (i, strength) in cdef_uv_strengths.iter_mut().enumerate() {
            *strength = u8::try_from(request.frame.cdef_params.cdef_uv_pri_strength[i])?;
        }

        let yframe_restoration_type =
            request.frame.loop_restoration_params.frame_restoration_type[0] as u16;
        let cbframe_restoration_type =
            request.frame.loop_restoration_params.frame_restoration_type[1] as u16;
        let crframe_restoration_type =
            request.frame.loop_restoration_params.frame_restoration_type[2] as u16;

        let lr_unit_shift = u16::from(request.frame.loop_restoration_params.lr_unit_shift);
        let lr_uv_shift = request.frame.loop_restoration_params.lr_uv_shift != 0;

        // Warped motion params
        let wm = [Default::default(); REFS_PER_FRAME];

        // Bit offsets of key syntax elements within the packed frame header data.
        // Per VA-API spec, offsets are "from the start of the packed header data"
        // which includes the OBU header and 4-byte OBU size field.
        // These are needed by the driver for rate control (to modify QP, CDEF, etc.).
        // When packed headers are not used, all offsets are 0.
        let mut bit_offset_qindex = 0u32;
        let bit_offset_segmentation = 0u32;
        let mut bit_offset_loopfilter_params = 0u32;
        let mut bit_offset_cdef_params = 0u32;
        let mut size_in_bits_cdef_params = 0u32;
        let mut byte_offset_frame_hdr_obu_size = 0u32;
        let mut size_in_bits_frame_hdr_obu = 0u32;

        if use_packed_headers {
            let obu_header_bytes: u32 = if request.frame.obu_header.extension_flag { 2 } else { 1 };
            const OBU_SIZE_BYTES: u32 = 4; // VA-API requires 4-byte leb128 obu_size
            let prefix_bits = (obu_header_bytes + OBU_SIZE_BYTES) * 8;

            bit_offset_qindex = prefix_bits + fh_offsets.qindex_offset;
            bit_offset_loopfilter_params = prefix_bits + fh_offsets.loopfilter_offset;
            if fh_offsets.cdef_start_offset > 0 {
                bit_offset_cdef_params = prefix_bits + fh_offsets.cdef_start_offset;
            }
            size_in_bits_cdef_params = fh_offsets.cdef_param_size;
            // For IDR frames, the driver concatenates packed SH + FH data,
            // so byte_offset_frame_hdr_obu_size must include the SH data length.
            byte_offset_frame_hdr_obu_size = packed_seq_data_len as u32 + obu_header_bytes;
            size_in_bits_frame_hdr_obu = prefix_bits + fh_offsets.frame_hdr_data_bits;
        }

        let temporal_id = u8::try_from(request.frame.obu_header.temporal_id)?;
        let spatial_id = u8::try_from(request.frame.obu_header.spatial_id)?;

        const NUMBER_SKIP_FRAMES: u8 = 0;
        const SKIP_FRAMES_REDUCED_SIZE: i32 = 0;

        Ok(EncPictureParameterBufferAV1::new(
            frame_width_minus_1,
            frame_height_minus_1,
            reconstructed_frame,
            coded_buf,
            reference_frames,
            ref_frame_idx,
            HIERARCHICAL_LEVEL_PLUS1,
            primary_ref_frame,
            order_hint,
            refresh_frame_flags,
            &ref_frame_ctrl_l0,
            &ref_frame_ctrl_l1,
            &AV1EncPictureFlags::new(
                frame_type,
                request.frame.error_resilient_mode,
                request.frame.disable_cdf_update,
                request.frame.use_superres,
                request.frame.allow_high_precision_mv,
                request.frame.use_ref_frame_mvs,
                request.frame.disable_frame_end_update_cdf,
                request.frame.reduced_tx_set,
                ENABLE_FRAME_OBU,
                LONG_TERM_REFERENCE,
                DISABLE_FRAME_RECON,
                request.frame.allow_intrabc,
                PALETTE_MODE_ENABLE,
                ALLOW_SCREEN_CONTENT_TOOLS,
                FORCE_INTEGER_MV,
            ),
            SEG_ID_BLOCK_SIZE,
            NUM_TILE_GROUPS_MINUS1,
            temporal_id,
            filter_level,
            filter_level_u,
            filter_level_v,
            &AV1EncLoopFilterFlags::new(
                request.frame.loop_filter_params.loop_filter_sharpness,
                request.frame.loop_filter_params.loop_filter_delta_enabled,
                request.frame.loop_filter_params.loop_filter_delta_update,
            ),
            superres_scale_denominator,
            interpolation_filter,
            loop_filter_ref_deltas,
            request.frame.loop_filter_params.loop_filter_mode_deltas,
            base_qindex,
            y_dc_delta_q,
            u_dc_delta_q,
            u_ac_delta_q,
            v_dc_delta_q,
            v_ac_delta_q,
            min_base_qindex,
            max_base_qindex,
            &AV1EncQMatrixFlags::new(
                request.frame.quantization_params.using_qmatrix,
                qm_y,
                qm_u,
                qm_v,
            ),
            &AV1EncModeControlFlags::new(
                request.frame.quantization_params.delta_q_present,
                request.frame.quantization_params.delta_q_res,
                request.frame.loop_filter_params.delta_lf_present,
                request.frame.loop_filter_params.delta_lf_res as u32,
                request.frame.loop_filter_params.delta_lf_multi,
                tx_mode,
                REFERENCE_MODE,
                request.frame.skip_mode_present,
            ),
            &EncSegParamAV1::new(
                &EncSegParamFlagsAV1::new(
                    request.frame.segmentation_params.segmentation_enabled,
                    request.frame.segmentation_params.segmentation_update_map,
                    segmentation_temporal_update,
                ),
                SEGMENT_NUMBER,
                request.frame.segmentation_params.feature_data,
                feature_mask,
            ),
            tile_cols,
            tile_rows,
            width_in_sbs_minus_1,
            height_in_sbs_minus_1,
            context_update_tile_id,
            cdef_damping_minus_3,
            cdef_bits,
            cdef_y_strengths,
            cdef_uv_strengths,
            &AV1EncLoopRestorationFlags::new(
                yframe_restoration_type,
                cbframe_restoration_type,
                crframe_restoration_type,
                lr_unit_shift,
                lr_uv_shift,
            ),
            wm,
            bit_offset_qindex,
            bit_offset_segmentation,
            bit_offset_loopfilter_params,
            bit_offset_cdef_params,
            size_in_bits_cdef_params,
            byte_offset_frame_hdr_obu_size,
            size_in_bits_frame_hdr_obu,
            &AV1EncTileGroupObuHdrInfo::new(
                request.frame.obu_header.extension_flag,
                request.frame.obu_header.has_size_field,
                temporal_id,
                spatial_id,
            ),
            NUMBER_SKIP_FRAMES,
            SKIP_FRAMES_REDUCED_SIZE,
        ))
    }

    fn build_tile_group_param() -> EncTileGroupBufferAV1 {
        // Single tile is only supported for now.
        EncTileGroupBufferAV1::new(0, 0)
    }

    /// Build packed sequence header (sequence header OBU) for keyframes.
    fn build_packed_sequence_header(
        sequence: &SequenceHeaderObu,
    ) -> Result<(BufferType, BufferType)> {
        let mut buffer = Vec::new();
        Synthesizer::<'_, SequenceHeaderObu, _>::synthesize(sequence, &mut buffer)
            .map_err(|e| BackendError::Other(anyhow::anyhow!("{}", e)))?;
        let length_in_bits = (buffer.len() * 8) as u32;
        let packed_param = BufferType::EncPackedHeaderParameter(EncPackedHeaderParameter::new(
            EncPackedHeaderType::Sequence,
            length_in_bits,
            true, // has_emulation_bytes
        ));
        let packed_data = BufferType::EncPackedHeaderData(buffer);
        Ok((packed_param, packed_data))
    }

    /// Build packed frame header (frame header OBU) and return bit offsets.
    fn build_packed_frame_header(
        frame: &FrameHeaderObu,
        sequence: &SequenceHeaderObu,
    ) -> Result<(BufferType, BufferType, FrameHeaderBitOffsets)> {
        let mut buffer = Vec::new();
        let offsets = Synthesizer::<'_, FrameHeaderObu, _>::synthesize_with_offsets(
            frame,
            sequence,
            &mut buffer,
        )
        .map_err(|e| BackendError::Other(anyhow::anyhow!("{}", e)))?;
        let length_in_bits = (buffer.len() * 8) as u32;
        let packed_param = BufferType::EncPackedHeaderParameter(EncPackedHeaderParameter::new(
            EncPackedHeaderType::Picture,
            length_in_bits,
            true, // has_emulation_bytes
        ));
        let packed_data = BufferType::EncPackedHeaderData(buffer);
        Ok((packed_param, packed_data, offsets))
    }
}

impl<M, H> StatelessAV1EncoderBackend for VaapiBackend<M, H>
where
    M: SurfaceMemoryDescriptor,
    H: std::borrow::Borrow<Surface<M>> + 'static,
{
    fn encode_tile_group(
        &mut self,
        request: BackendRequest<Self::Picture, Self::Reconstructed>,
    ) -> StatelessBackendResult<(Self::ReconPromise, Self::CodedPromise)> {
        let coded_buf = self.new_coded_buffer(&request.tunings.rate_control)?;
        let recon = self.new_scratch_picture()?;

        // Use the sequence header's enable_cdef setting (always true since predictor enables it).
        let enable_cdef = request.sequence.enable_cdef;

        let seq_param = Self::build_seq_param(&request, enable_cdef)?;
        let seq_param =
            libva::BufferType::EncSequenceParameter(libva::EncSequenceParameter::AV1(seq_param));

        let tg_param = Self::build_tile_group_param();
        let tg_param =
            libva::BufferType::EncSliceParameter(libva::EncSliceParameter::AV1(tg_param));

        let mut references = Vec::new();

        for ref_frame in &request.references {
            let Some(ref_frame) = ref_frame else {
                continue;
            };

            references.push(ref_frame.clone() as Rc<dyn std::any::Any>);
        }

        // Build packed headers if the driver supports them.
        // We build packed_pic first to get bit offsets needed by the picture parameter.
        let is_keyframe = matches!(request.frame.frame_type, FrameType::KeyFrame);

        let packed_seq = if is_keyframe {
            if self.supports_packed_header(libva::VA_ENC_PACKED_HEADER_SEQUENCE) {
                Some(Self::build_packed_sequence_header(&request.sequence)?)
            } else {
                None
            }
        } else {
            None
        };

        let (packed_pic, fh_offsets) =
            if self.supports_packed_header(libva::VA_ENC_PACKED_HEADER_PICTURE) {
                let (param, data, offsets) =
                    Self::build_packed_frame_header(&request.frame, &request.sequence)?;
                (Some((param, data)), offsets)
            } else {
                (None, Default::default())
            };

        let use_packed_headers = packed_seq.is_some() || packed_pic.is_some();

        // For IDR frames, the driver concatenates packed SH + FH data,
        // so byte_offset_frame_hdr_obu_size must include the SH data length.
        let packed_seq_data_len = packed_seq.as_ref().map_or(0, |(_, data)| {
            if let BufferType::EncPackedHeaderData(bytes) = data {
                bytes.len()
            } else {
                0
            }
        });

        let pic_param = Self::build_pic_param(
            &request,
            &recon,
            &coded_buf,
            &fh_offsets,
            packed_seq_data_len,
            use_packed_headers,
        )?;
        let pic_param =
            libva::BufferType::EncPictureParameter(libva::EncPictureParameter::AV1(pic_param));

        let mut picture =
            Picture::new(request.input_meta.timestamp, Rc::clone(self.context()), request.input);

        // Buffer ordering: Seq, RC, HRD, FrameRate, Pic, PackedSeq, PackedPic, TileGroup
        picture.add_buffer(self.context().create_buffer(seq_param)?);

        let rc_param =
            tunings_to_libva_rc::<{ MIN_BASE_QINDEX }, { MAX_BASE_QINDEX }>(&request.tunings)?;
        let rc_param =
            libva::BufferType::EncMiscParameter(libva::EncMiscParameter::RateControl(rc_param));
        picture.add_buffer(self.context().create_buffer(rc_param)?);

        // For rate-controlled modes, always send HRD parameters.
        // Use explicit rc_buffer_size if set, otherwise default to max bitrate.
        let hrd_buffer_size = request
            .tunings
            .rc_buffer_size
            .or_else(|| request.tunings.rate_control.bitrate_maximum().map(|b| b as u32));
        if let Some(hrd_buffer_size) = hrd_buffer_size {
            let hrd_buffer_fullness = hrd_buffer_size * 3 / 4;

            let hrd_param = libva::BufferType::EncMiscParameter(libva::EncMiscParameter::HRD(
                libva::EncMiscParameterHRD::new(hrd_buffer_fullness, hrd_buffer_size),
            ));
            picture.add_buffer(self.context().create_buffer(hrd_param)?);
        }

        let framerate_param =
            libva::BufferType::EncMiscParameter(libva::EncMiscParameter::FrameRate(
                libva::EncMiscParameterFrameRate::new(request.tunings.framerate, 0),
            ));
        picture.add_buffer(self.context().create_buffer(framerate_param)?);
        picture.add_buffer(self.context().create_buffer(pic_param)?);

        if let Some((packed_param, packed_data)) = packed_seq {
            picture.add_buffer(self.context().create_buffer(packed_param)?);
            picture.add_buffer(self.context().create_buffer(packed_data)?);
        }

        if let Some((packed_param, packed_data)) = packed_pic {
            picture.add_buffer(self.context().create_buffer(packed_param)?);
            picture.add_buffer(self.context().create_buffer(packed_data)?);
        }
        picture.add_buffer(self.context().create_buffer(tg_param)?);

        if let Some(max_frame_size) = request.tunings.max_frame_size {
            if self.supports_max_frame_size()? {
                let max_frame_size_param =
                    libva::BufferType::EncMiscParameter(libva::EncMiscParameter::MaxFrameSize(
                        libva::EncMiscParameterBufferMaxFrameSize::new(max_frame_size),
                    ));
                picture.add_buffer(self.context().create_buffer(max_frame_size_param)?);
            }
        }

        if let Some(quality) = request.tunings.quality {
            if self.supports_quality_range(quality)? {
                let quality_param =
                    libva::BufferType::EncMiscParameter(libva::EncMiscParameter::QualityLevel(
                        libva::EncMiscParameterBufferQualityLevel::new(quality),
                    ));
                picture.add_buffer(self.context().create_buffer(quality_param)?);
            }
        }

        // Start processing the picture encoding
        let picture = picture.begin().map_err(BackendError::BeginPictureError)?;
        let picture = picture.render().map_err(BackendError::RenderPictureError)?;
        let picture = picture.end().map_err(BackendError::EndPictureError)?;

        // libva will handle the synchronization of reconstructed surface with implicit fences.
        // Therefore return the reconstructed frame immediately.
        let reference_promise = ReadyPromise::from(recon);

        // When using packed headers, the driver includes the headers in its output.
        // We only need to prepend the temporal delimiter OBU.
        // When NOT using packed headers, use the full coded_output from the predictor
        // (which contains TD + SH + FH).
        let coded_output = if use_packed_headers {
            use crate::codec::av1::parser::ObuHeader;
            use crate::codec::av1::parser::ObuType;
            let mut td_buf = Vec::new();
            let td = TemporalDelimiterObu {
                obu_header: ObuHeader {
                    obu_type: ObuType::TemporalDelimiter,
                    extension_flag: false,
                    has_size_field: true,
                    temporal_id: 0,
                    spatial_id: 0,
                },
            };
            Synthesizer::<'_, TemporalDelimiterObu, _>::synthesize(&td, &mut td_buf)
                .map_err(|e| StatelessBackendError::Other(anyhow::anyhow!("{}", e)))?;
            td_buf
        } else {
            request.coded_output
        };

        let bitstream_promise =
            CodedOutputPromise::new(picture, references, coded_buf, coded_output);

        Ok((reference_promise, bitstream_promise))
    }
}

/// Create the VAAPI backend and query AV1 encoder features from the driver.
/// Shared setup logic for both `new_vaapi` and `new_native_vaapi`.
fn create_av1_vaapi_backend<D, S>(
    display: Arc<libva::Display>,
    config: &EncoderConfig,
    fourcc: Fourcc,
    coded_size: Resolution,
    low_power: bool,
) -> EncodeResult<(VaapiBackend<D, S>, EncoderFeaturesAV1)>
where
    D: SurfaceMemoryDescriptor,
    S: std::borrow::Borrow<Surface<D>> + 'static,
{
    let va_profile = match config.profile {
        Profile::Profile0 => VAProfileAV1Profile0,
        Profile::Profile1 => VAProfileAV1Profile1,
        _ => return Err(StatelessBackendError::UnsupportedProfile.into()),
    };

    let bitrate_control = match config.initial_tunings.rate_control {
        RateControl::ConstantBitrate(_) => libva::VA_RC_CBR,
        RateControl::VariableBitrate { .. } => libva::VA_RC_VBR,
        RateControl::ConstantQuality(_) => libva::VA_RC_CQP,
    };

    let backend =
        VaapiBackend::new(display, va_profile, fourcc, coded_size, bitrate_control, low_power)?;

    let av1_features = backend.query_av1_enc_features()?;

    Ok((backend, av1_features))
}

impl<V: VideoFrame>
    StatelessEncoder<AV1, V, VaapiBackend<V::MemDescriptor, Surface<V::MemDescriptor>>>
{
    pub fn new_vaapi(
        display: Arc<libva::Display>,
        config: EncoderConfig,
        fourcc: Fourcc,
        coded_size: Resolution,
        low_power: bool,
        blocking_mode: BlockingMode,
    ) -> EncodeResult<Self> {
        let (backend, av1_features) =
            create_av1_vaapi_backend(display, &config, fourcc, coded_size, low_power)?;
        Self::new_av1(backend, config, blocking_mode, av1_features)
    }
}

impl<D: SurfaceMemoryDescriptor, S: std::borrow::Borrow<Surface<D>> + 'static>
    StatelessEncoder<AV1, S, VaapiBackend<D, S>>
{
    pub fn new_native_vaapi(
        display: Arc<libva::Display>,
        config: EncoderConfig,
        fourcc: Fourcc,
        coded_size: Resolution,
        low_power: bool,
        blocking_mode: BlockingMode,
    ) -> EncodeResult<Self> {
        let (backend, av1_features) =
            create_av1_vaapi_backend(display, &config, fourcc, coded_size, low_power)?;
        Self::new_av1(backend, config, blocking_mode, av1_features)
    }
}

#[cfg(test)]
mod tests {
    use libva::Display;
    use libva::UsageHint;
    use libva::VAEntrypoint::VAEntrypointEncSliceLP;
    use libva::VAProfile::VAProfileAV1Profile0;
    use libva::VA_RT_FORMAT_YUV420;
    use libva::VA_RT_FORMAT_YUV420_10;

    use super::*;
    use crate::backend::vaapi::encoder::tests::upload_test_frame_nv12;
    use crate::backend::vaapi::encoder::tests::TestFrameGenerator;
    use crate::backend::vaapi::surface_pool::PooledVaSurface;
    use crate::backend::vaapi::surface_pool::VaSurfacePool;
    use crate::bitstream_utils::IvfFileHeader;
    use crate::bitstream_utils::IvfFrameHeader;
    use crate::codec::av1::parser::BitDepth;
    use crate::codec::av1::parser::CdefParams;
    use crate::codec::av1::parser::ColorConfig;
    use crate::codec::av1::parser::FrameHeaderObu;
    use crate::codec::av1::parser::FrameType;
    use crate::codec::av1::parser::ObuHeader;
    use crate::codec::av1::parser::ObuType;
    use crate::codec::av1::parser::OperatingPoint;
    use crate::codec::av1::parser::QuantizationParams;
    use crate::codec::av1::parser::SequenceHeaderObu;
    use crate::codec::av1::parser::TemporalDelimiterObu;
    use crate::codec::av1::parser::TileInfo;
    use crate::codec::av1::parser::TxMode;
    use crate::codec::av1::parser::MAX_NUM_OPERATING_POINTS;
    use crate::codec::av1::parser::PRIMARY_REF_NONE;
    use crate::codec::av1::parser::SELECT_INTEGER_MV;
    use crate::codec::av1::parser::SUPERRES_NUM;
    use crate::codec::av1::synthesizer::Synthesizer;
    use crate::decoder::FramePool;
    use crate::encoder::simple_encode_loop;
    use crate::encoder::stateless::BackendPromise;
    use crate::encoder::stateless::StatelessEncoderBackendImport;
    use crate::encoder::FrameMetadata;
    use crate::encoder::RateControl;
    use crate::encoder::Tunings;
    use crate::FrameLayout;
    use crate::PlaneLayout;
    use crate::Resolution;

    #[test]
    // Ignore this test by default as it requires libva-compatible hardware.
    #[ignore]
    fn test_single_frame() {
        let _ = env_logger::try_init();

        type Descriptor = ();
        type Surface = libva::Surface<Descriptor>;
        const WIDTH: u32 = 512;
        const HEIGHT: u32 = 512;
        let fourcc = b"NV12".into();

        let frame_layout = FrameLayout {
            format: (fourcc, 0),
            size: Resolution { width: WIDTH, height: HEIGHT },
            planes: vec![
                PlaneLayout { buffer_index: 0, offset: 0, stride: WIDTH as usize },
                PlaneLayout {
                    buffer_index: 0,
                    offset: (WIDTH * HEIGHT) as usize,
                    stride: WIDTH as usize,
                },
            ],
        };

        let display = Display::open().unwrap();
        let entrypoints = display.query_config_entrypoints(VAProfileAV1Profile0).unwrap();
        let low_power = entrypoints.contains(&VAEntrypointEncSliceLP);

        let mut backend = VaapiBackend::<Descriptor, Surface>::new(
            Arc::clone(&display),
            VAProfileAV1Profile0,
            fourcc,
            Resolution { width: WIDTH, height: HEIGHT },
            libva::VA_RC_CQP,
            low_power,
        )
        .unwrap();

        let mut surfaces = display
            .create_surfaces(
                VA_RT_FORMAT_YUV420,
                Some(frame_layout.format.0 .0),
                WIDTH,
                HEIGHT,
                Some(UsageHint::USAGE_HINT_ENCODER),
                vec![()],
            )
            .unwrap();

        let surface = surfaces.pop().unwrap();

        upload_test_frame_nv12(&display, &surface, 0.0);

        let input_meta = FrameMetadata {
            layout: frame_layout,
            force_keyframe: false,
            timestamp: 0,
            force_idr: false,
        };

        let input = backend.import_picture(&input_meta, surface).unwrap();

        let seq = SequenceHeaderObu {
            obu_header: ObuHeader {
                obu_type: ObuType::SequenceHeader,
                extension_flag: false,
                has_size_field: true,
                temporal_id: 0,
                spatial_id: 0,
            },

            seq_profile: Profile::Profile0,
            num_planes: 3,

            frame_width_bits_minus_1: 16 - 1,
            frame_height_bits_minus_1: 16 - 1,
            max_frame_width_minus_1: (WIDTH - 1) as u16,
            max_frame_height_minus_1: (HEIGHT - 1) as u16,

            enable_order_hint: true,
            order_hint_bits: 8,
            order_hint_bits_minus_1: 7,
            seq_force_integer_mv: SELECT_INTEGER_MV as u32,

            operating_points: {
                let mut ops: [OperatingPoint; MAX_NUM_OPERATING_POINTS] = Default::default();
                ops[0].seq_level_idx = 7;
                ops
            },

            color_config: ColorConfig {
                subsampling_x: true,
                subsampling_y: true,
                ..Default::default()
            },

            ..Default::default()
        };

        let frame = FrameHeaderObu {
            obu_header: ObuHeader {
                obu_type: ObuType::FrameHeader,
                extension_flag: false,
                has_size_field: true,
                temporal_id: 0,
                spatial_id: 0,
            },

            frame_type: FrameType::KeyFrame,
            frame_is_intra: true,
            primary_ref_frame: PRIMARY_REF_NONE,
            refresh_frame_flags: 0xff,
            error_resilient_mode: true,

            reduced_tx_set: true,
            tx_mode_select: 1,
            tx_mode: TxMode::Select,

            quantization_params: QuantizationParams { base_q_idx: 128, ..Default::default() },
            tile_info: TileInfo {
                uniform_tile_spacing_flag: true,
                tile_cols: 1,
                tile_rows: 1,
                tile_cols_log2: 0,
                tile_rows_log2: 0,
                width_in_sbs_minus_1: {
                    let mut value = [0u32; MAX_TILE_COLS];
                    value[0] = WIDTH / 64 - 1;
                    value
                },
                height_in_sbs_minus_1: {
                    let mut value = [0u32; MAX_TILE_ROWS];
                    value[0] = HEIGHT / 64 - 1;
                    value
                },
                ..Default::default()
            },
            cdef_params: CdefParams { cdef_damping: 3, ..Default::default() },
            superres_denom: SUPERRES_NUM as u32,
            upscaled_width: WIDTH,
            frame_width: WIDTH,
            frame_height: HEIGHT,
            render_width: WIDTH,
            render_height: HEIGHT,

            ..Default::default()
        };

        let request = Request {
            sequence: seq.clone(),
            frame: frame.clone(),
            input,
            input_meta,
            references: [None, None, None, None, None, None, None],
            ref_frame_ctrl_l0: [ReferenceFrameType::Intra; REFS_PER_FRAME],
            ref_frame_ctrl_l1: [ReferenceFrameType::Intra; REFS_PER_FRAME],
            intra_period: 4,
            ip_period: 1,
            tunings: Default::default(),
            coded_output: Vec::new(),
        };

        let (_, output) = backend.encode_tile_group(request).unwrap();
        let output = output.sync().unwrap();

        let write_to_file = std::option_env!("CROS_CODECS_TEST_WRITE_TO_FILE") == Some("true");
        if write_to_file {
            use std::io::Write;

            let mut out = std::fs::File::create("test_single_frame.av1.ivf").unwrap();

            let td = TemporalDelimiterObu {
                obu_header: ObuHeader {
                    obu_type: ObuType::TemporalDelimiter,
                    extension_flag: false,
                    has_size_field: true,
                    temporal_id: 0,
                    spatial_id: 0,
                },
            };

            let file_header =
                IvfFileHeader::new(IvfFileHeader::CODEC_AV1, WIDTH as u16, HEIGHT as u16, 30, 10);

            file_header.writo_into(&mut out).unwrap();

            {
                let mut hdr_buf = Vec::new();

                Synthesizer::<'_, TemporalDelimiterObu, _>::synthesize(&td, &mut hdr_buf).unwrap();
                Synthesizer::<'_, SequenceHeaderObu, _>::synthesize(&seq, &mut hdr_buf).unwrap();
                Synthesizer::<'_, FrameHeaderObu, _>::synthesize(&frame, &seq, &mut hdr_buf)
                    .unwrap();

                let frame_header = IvfFrameHeader {
                    frame_size: hdr_buf.len() as u32 + output.len() as u32,
                    timestamp: 0,
                };

                frame_header.writo_into(&mut out).unwrap();
                out.write_all(&hdr_buf).unwrap();
                out.write_all(&output).unwrap();
            }

            out.flush().unwrap();
        }
    }

    #[test]
    // Ignore this test by default as it requires libva-compatible hardware.
    #[ignore]
    fn test_vaapi_encoder() {
        type VaapiAv1Encoder<'l> =
            StatelessEncoder<AV1, PooledVaSurface<()>, VaapiBackend<(), PooledVaSurface<()>>>;

        const WIDTH: usize = 512;
        const HEIGHT: usize = 512;
        const FRAME_COUNT: u64 = 100;

        let _ = env_logger::try_init();

        let display = libva::Display::open().unwrap();
        let entrypoints = display.query_config_entrypoints(VAProfileAV1Profile0).unwrap();
        let low_power = entrypoints.contains(&VAEntrypointEncSliceLP);

        let config = EncoderConfig {
            profile: Profile::Profile0,
            resolution: Resolution { width: WIDTH as u32, height: HEIGHT as u32 },
            initial_tunings: Tunings {
                rate_control: RateControl::ConstantQuality(128),
                framerate: 30,
                ..Default::default()
            },
            ..Default::default()
        };

        let frame_layout = FrameLayout {
            format: (b"NV12".into(), 0),
            size: Resolution { width: WIDTH as u32, height: HEIGHT as u32 },
            planes: vec![
                PlaneLayout { buffer_index: 0, offset: 0, stride: WIDTH },
                PlaneLayout { buffer_index: 0, offset: WIDTH * HEIGHT, stride: WIDTH },
            ],
        };

        let mut encoder = VaapiAv1Encoder::new_vaapi(
            Arc::clone(&display),
            config,
            frame_layout.format.0,
            frame_layout.size,
            low_power,
            BlockingMode::Blocking,
        )
        .unwrap();

        let mut pool = VaSurfacePool::new(
            Arc::clone(&display),
            VA_RT_FORMAT_YUV420,
            Some(UsageHint::USAGE_HINT_ENCODER),
            Resolution { width: WIDTH as u32, height: HEIGHT as u32 },
        );

        pool.add_frames(vec![(); 16]).unwrap();

        let mut frame_producer = TestFrameGenerator::new(FRAME_COUNT, display, pool, frame_layout);

        let mut bitstream = Vec::new();

        let file_header = IvfFileHeader::new(
            IvfFileHeader::CODEC_AV1,
            WIDTH as u16,
            HEIGHT as u16,
            30,
            FRAME_COUNT as u32,
        );

        file_header.writo_into(&mut bitstream).unwrap();

        simple_encode_loop(&mut encoder, &mut frame_producer, |coded| {
            let header = IvfFrameHeader {
                timestamp: coded.metadata.timestamp,
                frame_size: coded.bitstream.len() as u32,
            };

            header.writo_into(&mut bitstream).unwrap();
            bitstream.extend(coded.bitstream);
        })
        .unwrap();

        let write_to_file = std::option_env!("CROS_CODECS_TEST_WRITE_TO_FILE") == Some("true");
        if write_to_file {
            use std::io::Write;
            let mut out = std::fs::File::create("test_vaapi_encoder.av1.ivf").unwrap();
            out.write_all(&bitstream).unwrap();
            out.flush().unwrap();
        }
    }

    #[ignore]
    // Ignore this test by default as it requires libva-compatible hardware.
    #[test]
    fn test_vaapi_encoder_p010() {
        type VaapiAv1Encoder<'l> =
            StatelessEncoder<AV1, PooledVaSurface<()>, VaapiBackend<(), PooledVaSurface<()>>>;

        const WIDTH: usize = 512;
        const HEIGHT: usize = 512;
        const FRAME_COUNT: u64 = 100;

        let _ = env_logger::try_init();

        let display = libva::Display::open().unwrap();
        let entrypoints = display.query_config_entrypoints(VAProfileAV1Profile0).unwrap();
        let low_power = entrypoints.contains(&VAEntrypointEncSliceLP);

        let config = EncoderConfig {
            profile: Profile::Profile0,
            resolution: Resolution { width: WIDTH as u32, height: HEIGHT as u32 },
            bit_depth: BitDepth::Depth10,
            initial_tunings: Tunings {
                rate_control: RateControl::ConstantQuality(128),
                framerate: 30,
                ..Default::default()
            },
            ..Default::default()
        };

        let frame_layout = FrameLayout {
            format: (b"P010".into(), 0),
            size: Resolution { width: WIDTH as u32, height: HEIGHT as u32 },
            planes: vec![
                PlaneLayout { buffer_index: 0, offset: 0, stride: WIDTH },
                PlaneLayout { buffer_index: 0, offset: WIDTH * HEIGHT, stride: WIDTH },
            ],
        };

        let mut encoder = VaapiAv1Encoder::new_vaapi(
            Arc::clone(&display),
            config,
            frame_layout.format.0,
            frame_layout.size,
            low_power,
            BlockingMode::Blocking,
        )
        .unwrap();

        let mut pool = VaSurfacePool::new(
            Arc::clone(&display),
            VA_RT_FORMAT_YUV420_10,
            Some(UsageHint::USAGE_HINT_ENCODER),
            Resolution { width: WIDTH as u32, height: HEIGHT as u32 },
        );

        pool.add_frames(vec![(); 16]).unwrap();

        let mut frame_producer = TestFrameGenerator::new(FRAME_COUNT, display, pool, frame_layout);

        let mut bitstream = Vec::new();

        let file_header = IvfFileHeader::new(
            IvfFileHeader::CODEC_AV1,
            WIDTH as u16,
            HEIGHT as u16,
            30,
            FRAME_COUNT as u32,
        );

        file_header.writo_into(&mut bitstream).unwrap();

        simple_encode_loop(&mut encoder, &mut frame_producer, |coded| {
            let header = IvfFrameHeader {
                timestamp: coded.metadata.timestamp,
                frame_size: coded.bitstream.len() as u32,
            };

            header.writo_into(&mut bitstream).unwrap();
            bitstream.extend(coded.bitstream);
        })
        .unwrap();

        let write_to_file = std::option_env!("CROS_CODECS_TEST_WRITE_TO_FILE") == Some("true");
        if write_to_file {
            use std::io::Write;
            let mut out = std::fs::File::create("test_vaapi_encoder_p010.av1.ivf").unwrap();
            out.write_all(&bitstream).unwrap();
            out.flush().unwrap();
        }
    }
}

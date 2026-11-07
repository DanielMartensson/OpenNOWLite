// Copyright 2024 The ChromiumOS Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use crate::codec::av1::parser::BitDepth;
use crate::codec::av1::parser::CdefParams;
use crate::codec::av1::parser::ColorConfig;
use crate::codec::av1::parser::FrameHeaderObu;
use crate::codec::av1::parser::FrameType;
use crate::codec::av1::parser::ObuHeader;
use crate::codec::av1::parser::ObuType;
use crate::codec::av1::parser::OperatingPoint;
use crate::codec::av1::parser::Profile;
use crate::codec::av1::parser::QuantizationParams;
use crate::codec::av1::parser::ReferenceFrameType;
use crate::codec::av1::parser::SequenceHeaderObu;
use crate::codec::av1::parser::TemporalDelimiterObu;
use crate::codec::av1::parser::TileInfo;
use crate::codec::av1::parser::TxMode;
use crate::codec::av1::parser::MAX_NUM_OPERATING_POINTS;
use crate::codec::av1::parser::MAX_TILE_COLS;
use crate::codec::av1::parser::MAX_TILE_ROWS;
use crate::codec::av1::parser::PRIMARY_REF_NONE;
use crate::codec::av1::parser::REFS_PER_FRAME;
use crate::codec::av1::parser::SELECT_INTEGER_MV;
use crate::codec::av1::parser::SUPERRES_NUM;
use crate::codec::av1::synthesizer::Synthesizer;
use crate::encoder::stateless::av1::BackendRequest;
use crate::encoder::stateless::av1::EncoderConfig;
use crate::encoder::stateless::predictor::LowDelay;
use crate::encoder::stateless::predictor::LowDelayDelegate;
use crate::encoder::EncodeResult;
use crate::encoder::FrameMetadata;
use crate::encoder::RateControl;
use crate::encoder::Tunings;

// AV1 Spec. Dc_Qlookup max indices
pub(crate) const MIN_BASE_QINDEX: u32 = 0;
pub(crate) const MAX_BASE_QINDEX: u32 = 255;

/// AV1 encoder features queried from the driver.
/// Used by the predictor to condition sequence/frame header parameters.
#[derive(Debug, Clone)]
pub(crate) struct EncoderFeaturesAV1 {
    pub support_128x128_superblock: bool,
    pub support_filter_intra: bool,
    pub support_intra_edge_filter: bool,
    pub support_interintra_compound: bool,
    pub support_masked_compound: bool,
    pub support_warped_motion: bool,
    pub support_dual_filter: bool,
    pub support_jnt_comp: bool,
    pub support_ref_frame_mvs: bool,
    pub support_superres: bool,
    pub support_restoration: bool,
    /// Bitmask of supported tx modes: bit 0 = ONLY_4X4, bit 1 = LARGEST, bit 2 = SELECT
    pub tx_mode_support: u8,
    #[allow(dead_code)]
    pub max_tile_num_minus1: u16,
}

impl Default for EncoderFeaturesAV1 {
    fn default() -> Self {
        Self {
            support_128x128_superblock: false,
            support_filter_intra: false,
            support_intra_edge_filter: false,
            support_interintra_compound: false,
            support_masked_compound: false,
            support_warped_motion: false,
            support_dual_filter: false,
            support_jnt_comp: false,
            support_ref_frame_mvs: false,
            support_superres: false,
            support_restoration: false,
            tx_mode_support: 0x04, // Assume TX_MODE_SELECT
            max_tile_num_minus1: 0,
        }
    }
}

pub(crate) struct LowDelayAV1Delegate {
    /// Current sequence header obu
    sequence: SequenceHeaderObu,

    /// Encoder config
    config: EncoderConfig,

    /// AV1 encoder features queried from the driver.
    features: EncoderFeaturesAV1,
}

pub(crate) type LowDelayAV1<Picture, Reference> =
    LowDelay<Picture, Reference, LowDelayAV1Delegate, BackendRequest<Picture, Reference>>;

impl<Picture, Reference> LowDelayAV1<Picture, Reference> {
    pub fn new(config: EncoderConfig, limit: u16, features: EncoderFeaturesAV1) -> Self {
        let sequence = Self::create_sequence_header(&config, &features);
        Self {
            queue: Default::default(),
            references: Default::default(),
            counter: 0,
            limit,
            tunings: config.initial_tunings.clone(),
            delegate: LowDelayAV1Delegate { sequence, config, features },
            tunings_queue: Default::default(),
            _phantom: Default::default(),
        }
    }

    fn create_sequence_header(
        config: &EncoderConfig,
        features: &EncoderFeaturesAV1,
    ) -> SequenceHeaderObu {
        let width = config.resolution.width;
        let height = config.resolution.height;

        SequenceHeaderObu {
            obu_header: ObuHeader {
                obu_type: ObuType::SequenceHeader,
                extension_flag: false,
                has_size_field: true,
                temporal_id: 0,
                spatial_id: 0,
            },

            seq_profile: Profile::Profile0,
            num_planes: 3,

            enable_order_hint: true,
            order_hint_bits: 8,
            order_hint_bits_minus_1: 8 - 1,

            // Use maximum size (16 bits)
            frame_width_bits_minus_1: (1 << 4) - 1,
            frame_height_bits_minus_1: (1 << 4) - 1,

            // Current resolution is the maximum resolution
            max_frame_width_minus_1: (width - 1) as u16,
            max_frame_height_minus_1: (height - 1) as u16,

            seq_force_integer_mv: SELECT_INTEGER_MV as u32,

            // Gate features based on driver capabilities
            use_128x128_superblock: features.support_128x128_superblock,
            enable_filter_intra: features.support_filter_intra,
            enable_intra_edge_filter: features.support_intra_edge_filter,
            enable_interintra_compound: features.support_interintra_compound,
            enable_masked_compound: features.support_masked_compound,
            enable_warped_motion: features.support_warped_motion,
            enable_dual_filter: features.support_dual_filter,
            enable_jnt_comp: features.support_jnt_comp,
            enable_ref_frame_mvs: features.support_ref_frame_mvs,
            enable_superres: features.support_superres,
            enable_cdef: true,
            enable_restoration: features.support_restoration,

            operating_points: {
                let mut ops: [OperatingPoint; MAX_NUM_OPERATING_POINTS] = Default::default();
                ops[0].idc = 0;
                // Let the driver pick the appropriate level by using 0 (2.0).
                // The driver will override if needed based on the actual encoding parameters.
                ops[0].seq_level_idx = 0;
                ops
            },

            bit_depth: config.bit_depth,
            color_config: ColorConfig {
                // YUV 4:2:0 8-bit or 10-bit
                high_bitdepth: config.bit_depth == BitDepth::Depth10,
                mono_chrome: false,
                subsampling_x: true,
                subsampling_y: true,
                ..Default::default()
            },

            ..Default::default()
        }
    }

    fn create_temporal_delimiter() -> TemporalDelimiterObu {
        TemporalDelimiterObu {
            obu_header: ObuHeader {
                obu_type: ObuType::TemporalDelimiter,
                extension_flag: false,
                has_size_field: true,
                temporal_id: 0,
                spatial_id: 0,
            },
        }
    }

    fn create_frame_header(&self, frame_type: FrameType) -> EncodeResult<FrameHeaderObu> {
        let width = self.delegate.config.resolution.width;
        let height = self.delegate.config.resolution.height;
        let features = &self.delegate.features;

        // Superblock size
        let sb_size = if self.delegate.sequence.use_128x128_superblock { 128 } else { 64 };

        // Use frame counter for order hinting
        let order_hint_mask = (1 << self.delegate.sequence.order_hint_bits) - 1;
        let order_hint = (self.counter & order_hint_mask) as u32;

        // Clamp tunings's quality range to correct range
        let min_q_idx = self.tunings.min_quality.max(MIN_BASE_QINDEX);
        let max_q_idx = self.tunings.max_quality.min(MAX_BASE_QINDEX);

        // For CQP mode, use the specified QP. For bitrate-controlled modes (VBR/CBR),
        // the driver controls QP so we use a sensible default initial QP.
        let base_q_idx = match self.tunings.rate_control {
            RateControl::ConstantQuality(qp) => qp.clamp(min_q_idx, max_q_idx),
            _ => (min_q_idx + max_q_idx) / 2,
        };

        // Set the frame size in superblocks for the only tile
        let mut width_in_sbs_minus_1 = [0u32; MAX_TILE_COLS];
        width_in_sbs_minus_1[0] = ((width + sb_size - 1) / sb_size) - 1;

        let mut height_in_sbs_minus_1 = [0u32; MAX_TILE_ROWS];
        height_in_sbs_minus_1[0] = ((height + sb_size - 1) / sb_size) - 1;

        // Select tx_mode based on driver capabilities
        let tx_mode = if features.tx_mode_support & 0x04 != 0 {
            TxMode::Select
        } else if features.tx_mode_support & 0x02 != 0 {
            TxMode::Largest
        } else {
            log::warn!("No preferred tx mode supported by driver, falling back to Select");
            TxMode::Select
        };
        let tx_mode_select = if matches!(tx_mode, TxMode::Select) { 1 } else { 0 };

        Ok(FrameHeaderObu {
            obu_header: ObuHeader {
                obu_type: ObuType::FrameHeader,
                extension_flag: false,
                has_size_field: true,
                temporal_id: 0,
                spatial_id: 0,
            },
            show_frame: true,
            showable_frame: !matches!(frame_type, FrameType::KeyFrame),
            frame_type,
            frame_is_intra: matches!(frame_type, FrameType::KeyFrame | FrameType::IntraOnlyFrame),
            primary_ref_frame: PRIMARY_REF_NONE,
            refresh_frame_flags: if matches!(frame_type, FrameType::KeyFrame) {
                0xff
            } else {
                0x01
            },

            // Use error resilient mode, and provide the order hints for referencing frame ie. just
            // previous frame
            error_resilient_mode: true,
            order_hint,
            ref_order_hint: [0, 0, 0, 0, 0, 0, 0, 0],

            reduced_tx_set: false,
            tx_mode_select,
            tx_mode,

            // Provide the Q index from config
            quantization_params: QuantizationParams { base_q_idx, ..Default::default() },

            // Use single tile for now
            tile_info: TileInfo {
                uniform_tile_spacing_flag: true,
                tile_cols: 1,
                tile_rows: 1,
                tile_cols_log2: 0,
                tile_rows_log2: 0,
                width_in_sbs_minus_1,
                height_in_sbs_minus_1,
                ..Default::default()
            },

            // CDEF is not used currently, use default value to keep Synthesizer happy
            cdef_params: CdefParams { cdef_damping: 3, ..Default::default() },

            // No superres
            superres_denom: SUPERRES_NUM as u32,
            upscaled_width: width,
            frame_width: width,
            frame_height: height,
            render_width: width,
            render_height: height,

            ..Default::default()
        })
    }
}

impl<Picture, Reference> LowDelayDelegate<Picture, Reference, BackendRequest<Picture, Reference>>
    for LowDelayAV1<Picture, Reference>
{
    fn request_keyframe(
        &mut self,
        input: Picture,
        input_meta: FrameMetadata,
        idr: bool,
    ) -> EncodeResult<BackendRequest<Picture, Reference>> {
        log::trace!("Requested keyframe timestamp={}", input_meta.timestamp);

        let temporal_delim = Self::create_temporal_delimiter();
        let sequence = self.delegate.sequence.clone();
        let frame = self.create_frame_header(FrameType::KeyFrame)?;

        // This is intra frame, so there is no references
        let references = [None, None, None, None, None, None, None];
        let ref_frame_ctrl_l0 = [ReferenceFrameType::Intra; REFS_PER_FRAME];
        let ref_frame_ctrl_l1 = [ReferenceFrameType::Intra; REFS_PER_FRAME];

        let mut coded_output = Vec::new();

        // Output Temporal Delimiter, Sequence Header and Frame Header OBUs to bitstream
        Synthesizer::<'_, TemporalDelimiterObu, _>::synthesize(&temporal_delim, &mut coded_output)?;
        if idr {
            Synthesizer::<'_, SequenceHeaderObu, _>::synthesize(&sequence, &mut coded_output)?;
        }
        Synthesizer::<'_, FrameHeaderObu, _>::synthesize(&frame, &sequence, &mut coded_output)?;

        let request = BackendRequest {
            sequence,
            frame,
            input,
            input_meta,
            references,
            ref_frame_ctrl_l0,
            ref_frame_ctrl_l1,
            intra_period: self.limit as u32,
            ip_period: 1,
            tunings: self.tunings.clone(),
            coded_output,
        };

        Ok(request)
    }

    fn request_interframe(
        &mut self,
        input: Picture,
        input_meta: FrameMetadata,
    ) -> EncodeResult<BackendRequest<Picture, Reference>> {
        log::trace!("Requested interframe timestamp={}", input_meta.timestamp);

        let temporal_delim = Self::create_temporal_delimiter();
        let sequence = self.delegate.sequence.clone();
        let mut frame = self.create_frame_header(FrameType::InterFrame)?;

        // Use previous frame as last frame reference
        let references = [self.references.front().cloned(), None, None, None, None, None, None];

        let order_hint_mask = (1 << self.delegate.sequence.order_hint_bits) - 1;
        let mut ref_frame_ctrl_l0 = [ReferenceFrameType::Intra; REFS_PER_FRAME];
        let ref_frame_ctrl_l1 = [ReferenceFrameType::Intra; REFS_PER_FRAME];

        // Enable previous frame as reference
        ref_frame_ctrl_l0[0] = ReferenceFrameType::Last;
        frame.ref_frame_idx[0] = 0;
        frame.last_frame_idx = 0;
        frame.ref_order_hint[0] = ((self.counter - 1) & order_hint_mask) as u32;

        let mut coded_output = Vec::new();

        // Output Temporal Delimiter and Frame Header OBUs to bitstream, marking next frame
        Synthesizer::<'_, TemporalDelimiterObu, _>::synthesize(&temporal_delim, &mut coded_output)?;
        Synthesizer::<'_, FrameHeaderObu, _>::synthesize(&frame, &sequence, &mut coded_output)?;

        let request = BackendRequest {
            sequence,
            frame,
            input,
            input_meta,
            references,
            ref_frame_ctrl_l0,
            ref_frame_ctrl_l1,
            intra_period: self.limit as u32,
            ip_period: 1,
            tunings: self.tunings.clone(),
            coded_output,
        };

        self.references.clear();

        Ok(request)
    }

    fn try_tunings(&self, _tunings: &Tunings) -> EncodeResult<()> {
        Ok(())
    }

    fn apply_tunings(&mut self, _tunings: &Tunings) -> EncodeResult<()> {
        Ok(())
    }
}

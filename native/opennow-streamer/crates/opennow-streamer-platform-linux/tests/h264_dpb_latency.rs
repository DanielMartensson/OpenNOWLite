#![cfg(feature = "vaapi")]

use std::rc::Rc;
use std::sync::{Arc, Mutex};

use cros_codecs::codec::h264::dpb::{Dpb, DpbEntry};
use cros_codecs::codec::h264::parser::{
    Pps, PpsBuilder, RefPicMarkingInner, Slice, SliceHeader, SliceHeaderBuilder, SliceType, Sps,
    SpsBuilder,
};
use cros_codecs::codec::h264::picture::PictureData;
use cros_codecs::codec::h264::synthesizer::Synthesizer;
use cros_codecs::decoder::stateless::h264::{H264, StatelessH264DecoderBackend};
use cros_codecs::decoder::stateless::{
    DecodeError, NewPictureResult, StatelessBackendResult, StatelessDecoder,
    StatelessDecoderBackend, StatelessDecoderBackendPicture, StatelessVideoDecoder,
};
use cros_codecs::decoder::{BlockingMode, DecodedHandle, DecoderEvent, StreamInfo};
use cros_codecs::encoder::stateless::h264::IsReference;
use cros_codecs::libva::{Display, Surface};
use cros_codecs::video_frame::{ReadMapping, VideoFrame, WriteMapping};
use cros_codecs::{DecodedFormat, Fourcc, Resolution};

#[derive(Debug)]
struct TestFrame;

impl VideoFrame for TestFrame {
    type MemDescriptor = ();
    type NativeHandle = Surface<()>;

    fn fourcc(&self) -> Fourcc {
        Fourcc::from(b"NV12")
    }

    fn resolution(&self) -> Resolution {
        Resolution::from((64, 64))
    }

    fn get_plane_size(&self) -> Vec<usize> {
        vec![4096, 2048]
    }

    fn get_plane_pitch(&self) -> Vec<usize> {
        vec![64, 64]
    }

    fn map<'a>(&'a self) -> Result<Box<dyn ReadMapping<'a> + 'a>, String> {
        Err("test frame has no pixel storage".into())
    }

    fn map_mut<'a>(&'a mut self) -> Result<Box<dyn WriteMapping<'a> + 'a>, String> {
        Err("test frame has no pixel storage".into())
    }

    fn to_native_handle(&self, _: &Arc<Display>) -> Result<Self::NativeHandle, String> {
        Err("test frame has no GPU surface".into())
    }
}

#[derive(Clone)]
struct TestHandle {
    timestamp: u64,
    frame: Arc<TestFrame>,
}

impl DecodedHandle for TestHandle {
    type Frame = TestFrame;

    fn video_frame(&self) -> Arc<TestFrame> {
        Arc::clone(&self.frame)
    }

    fn timestamp(&self) -> u64 {
        self.timestamp
    }

    fn coded_resolution(&self) -> Resolution {
        self.frame.resolution()
    }

    fn display_resolution(&self) -> Resolution {
        self.frame.resolution()
    }

    fn is_ready(&self) -> bool {
        true
    }

    fn sync(&self) -> anyhow::Result<()> {
        Ok(())
    }
}

#[derive(Default)]
struct Observations {
    references: Vec<Vec<u64>>,
    submitted: Vec<u64>,
    resets: usize,
}

struct TestBackend {
    info: Option<StreamInfo>,
    observations: Arc<Mutex<Observations>>,
}

impl StatelessDecoderBackend for TestBackend {
    type Handle = TestHandle;

    fn stream_info(&self) -> Option<&StreamInfo> {
        self.info.as_ref()
    }

    fn reset_backend(&mut self) -> anyhow::Result<()> {
        self.observations.lock().unwrap().resets += 1;
        Ok(())
    }
}

impl StatelessDecoderBackendPicture<H264> for TestBackend {
    type Picture = TestHandle;
}

impl StatelessH264DecoderBackend for TestBackend {
    fn new_sequence(&mut self, sps: &Rc<Sps>) -> StatelessBackendResult<()> {
        self.info = Some(StreamInfo {
            format: DecodedFormat::NV12,
            coded_resolution: Resolution::from((sps.width(), sps.height())),
            display_resolution: Resolution::from((sps.width(), sps.height())),
            min_num_frames: sps.max_dpb_frames() + 2,
        });
        Ok(())
    }

    fn new_picture(
        &mut self,
        timestamp: u64,
        alloc_cb: &mut dyn FnMut() -> Option<TestFrame>,
    ) -> NewPictureResult<TestHandle> {
        Ok(TestHandle {
            timestamp,
            frame: Arc::new(alloc_cb().unwrap()),
        })
    }

    fn new_field_picture(
        &mut self,
        timestamp: u64,
        first_field: &TestHandle,
    ) -> NewPictureResult<TestHandle> {
        Ok(TestHandle {
            timestamp,
            frame: first_field.video_frame(),
        })
    }

    fn start_picture(
        &mut self,
        _: &mut TestHandle,
        _: &PictureData,
        _: &Sps,
        _: &Pps,
        dpb: &Dpb<TestHandle>,
        _: &SliceHeader,
    ) -> StatelessBackendResult<()> {
        self.observations.lock().unwrap().references.push(
            dpb.entries()
                .iter()
                .filter_map(|entry| entry.reference.as_ref().map(DecodedHandle::timestamp))
                .collect(),
        );
        Ok(())
    }

    fn decode_slice(
        &mut self,
        _: &mut TestHandle,
        _: &Slice,
        _: &Sps,
        _: &Pps,
        _: &[&DpbEntry<TestHandle>],
        _: &[&DpbEntry<TestHandle>],
    ) -> StatelessBackendResult<()> {
        Ok(())
    }

    fn submit_picture(&mut self, picture: TestHandle) -> StatelessBackendResult<TestHandle> {
        self.observations
            .lock()
            .unwrap()
            .submitted
            .push(picture.timestamp);
        Ok(picture)
    }
}

fn feed(
    decoder: &mut StatelessDecoder<H264, TestBackend>,
    timestamp: u64,
    bytes: &[u8],
) -> Vec<u64> {
    let mut ready = vec![];
    let mut remaining = bytes;
    let mut event_retries = 0;
    while !remaining.is_empty() {
        match decoder.decode(timestamp, remaining, &mut || Some(TestFrame)) {
            Ok(consumed) => {
                assert!(consumed > 0);
                remaining = &remaining[consumed..];
            }
            Err(DecodeError::CheckEvents) => {
                event_retries += 1;
                assert!(
                    event_retries < 8,
                    "decoder did not leave format negotiation"
                );
            }
            Err(error) => panic!("decode failed: {error}"),
        }
        while let Some(event) = decoder.next_event() {
            if let DecoderEvent::FrameReady(handle) = event {
                ready.push(handle.timestamp());
            }
        }
    }
    ready
}

fn parameter_sets(reorder: u32) -> (Rc<Sps>, Rc<Pps>, Vec<u8>) {
    let sps = SpsBuilder::new()
        .resolution(64, 64)
        .frame_mbs_only_flag(true)
        .max_num_ref_frames(15)
        .max_dec_frame_buffering(15)
        .max_num_reorder_frames(reorder)
        .build();
    encode_parameters(sps)
}

fn encode_parameters(sps: Rc<Sps>) -> (Rc<Sps>, Rc<Pps>, Vec<u8>) {
    let pps = PpsBuilder::new(sps.clone()).build();
    let mut bytes = vec![];
    Synthesizer::<'_, Sps, _>::synthesize(3, &sps, &mut bytes, true).unwrap();
    Synthesizer::<'_, Pps, _>::synthesize(3, &pps, &mut bytes, true).unwrap();
    (sps, pps, bytes)
}

fn slice(sps: &Sps, pps: &Pps, index: u16, poc: u16, idr: bool) -> Vec<u8> {
    let header = SliceHeaderBuilder::new(pps)
        .slice_type(if idr { SliceType::I } else { SliceType::P })
        .frame_num(index)
        .pic_order_cnt_lsb(poc)
        .build();
    encode_slice(sps, pps, &header, idr, IsReference::ShortTerm)
}

fn encode_slice(
    sps: &Sps,
    pps: &Pps,
    header: &SliceHeader,
    idr: bool,
    reference: IsReference,
) -> Vec<u8> {
    let mut bytes = vec![];
    Synthesizer::<'_, SliceHeader, _>::synthesize(
        header, sps, pps, idr, reference, &mut bytes, true,
    )
    .unwrap();
    bytes.push(0x80);
    bytes
}

#[test]
fn progressive_zero_reorder_outputs_every_submitted_picture_and_retains_references() {
    for mode in [BlockingMode::Blocking, BlockingMode::NonBlocking] {
        let observations = Arc::new(Mutex::new(Observations::default()));
        let mut decoder = StatelessDecoder::<H264, _>::new(
            TestBackend {
                info: None,
                observations: Arc::clone(&observations),
            },
            mode,
        )
        .unwrap();
        let (sps, pps, parameters) = parameter_sets(0);
        assert!(feed(&mut decoder, 0, &parameters).is_empty());
        assert!(feed(&mut decoder, 100, &slice(&sps, &pps, 0, 0, true)).is_empty());
        for index in 1..41 {
            let ready = feed(
                &mut decoder,
                100 + u64::from(index),
                &slice(&sps, &pps, index % 16, (index * 2) % 16, false),
            );
            assert_eq!(ready, [99 + u64::from(index)]);
            let observed = observations.lock().unwrap();
            assert_eq!(observed.submitted.len(), index as usize);
            assert_eq!(
                observed.references.last().unwrap().len(),
                (index as usize).min(15)
            );
        }
    }
}

#[test]
fn adaptive_reference_marking_reclaims_already_output_pictures_before_storage() {
    for mode in [BlockingMode::Blocking, BlockingMode::NonBlocking] {
        let observations = Arc::new(Mutex::new(Observations::default()));
        let mut decoder = StatelessDecoder::<H264, _>::new(
            TestBackend {
                info: None,
                observations: Arc::clone(&observations),
            },
            mode,
        )
        .unwrap();
        let (sps, pps, parameters) = parameter_sets(0);
        feed(&mut decoder, 0, &parameters);
        feed(&mut decoder, 100, &slice(&sps, &pps, 0, 0, true));
        for index in 1..41 {
            let mut header = SliceHeaderBuilder::new(&pps)
                .slice_type(SliceType::P)
                .frame_num(index % 16)
                .pic_order_cnt_lsb((index * 2) % 16)
                .build();
            if index >= 15 {
                header
                    .dec_ref_pic_marking
                    .adaptive_ref_pic_marking_mode_flag = true;
                header.dec_ref_pic_marking.inner.push(RefPicMarkingInner {
                    memory_management_control_operation: 1,
                    difference_of_pic_nums_minus1: 14,
                    ..Default::default()
                });
            }
            let ready = feed(
                &mut decoder,
                100 + u64::from(index),
                &encode_slice(&sps, &pps, &header, false, IsReference::ShortTerm),
            );
            assert_eq!(ready, [99 + u64::from(index)]);
            assert_eq!(
                observations
                    .lock()
                    .unwrap()
                    .references
                    .last()
                    .unwrap()
                    .len(),
                (index as usize).min(15)
            );
        }
    }
}

#[test]
fn progressive_reordered_pictures_keep_the_sps_limit_across_idr_and_flush() {
    for mode in [BlockingMode::Blocking, BlockingMode::NonBlocking] {
        let observations = Arc::new(Mutex::new(Observations::default()));
        let mut decoder = StatelessDecoder::<H264, _>::new(
            TestBackend {
                info: None,
                observations,
            },
            mode,
        )
        .unwrap();
        let (sps, pps, parameters) = parameter_sets(2);
        assert!(feed(&mut decoder, 0, &parameters).is_empty());
        for epoch in 0..2 {
            let base = epoch * 100;
            for (index, poc) in [0, 6, 2, 4, 8].into_iter().enumerate() {
                let bytes = if matches!(poc, 2 | 4) {
                    let header = SliceHeaderBuilder::new(&pps)
                        .slice_type(SliceType::B)
                        .frame_num(2)
                        .pic_order_cnt_lsb(poc)
                        .build();
                    encode_slice(&sps, &pps, &header, false, IsReference::No)
                } else {
                    slice(
                        &sps,
                        &pps,
                        if poc == 8 { 2 } else { index as u16 },
                        poc,
                        index == 0,
                    )
                };
                let ready = feed(&mut decoder, base + u64::from(poc), &bytes);
                let expected = match index {
                    3 => vec![base],
                    4 => vec![base + 2],
                    _ => vec![],
                };
                assert_eq!(ready, expected);
            }
            decoder.flush().unwrap();
            let mut drained = vec![];
            while let Some(event) = decoder.next_event() {
                if let DecoderEvent::FrameReady(handle) = event {
                    drained.push(handle.timestamp());
                }
            }
            assert_eq!(drained, [base + 4, base + 6, base + 8]);
        }
    }
}

#[test]
fn idr_discard_and_mmco5_keep_reorder_bound_without_reusing_old_references() {
    for mode in [BlockingMode::Blocking, BlockingMode::NonBlocking] {
        let observations = Arc::new(Mutex::new(Observations::default()));
        let mut decoder = StatelessDecoder::<H264, _>::new(
            TestBackend {
                info: None,
                observations: Arc::clone(&observations),
            },
            mode,
        )
        .unwrap();
        let (sps, pps, parameters) = parameter_sets(2);
        feed(&mut decoder, 0, &parameters);
        feed(&mut decoder, 10, &slice(&sps, &pps, 0, 0, true));
        feed(&mut decoder, 12, &slice(&sps, &pps, 1, 2, false));
        let mut idr = SliceHeaderBuilder::new(&pps)
            .slice_type(SliceType::I)
            .idr_pic_id(1)
            .build();
        idr.dec_ref_pic_marking.no_output_of_prior_pics_flag = true;
        assert!(
            feed(
                &mut decoder,
                20,
                &encode_slice(&sps, &pps, &idr, true, IsReference::ShortTerm)
            )
            .is_empty()
        );
        assert!(
            observations
                .lock()
                .unwrap()
                .references
                .last()
                .unwrap()
                .is_empty()
        );
        assert!(feed(&mut decoder, 22, &slice(&sps, &pps, 1, 2, false)).is_empty());

        let mut mmco = SliceHeaderBuilder::new(&pps)
            .slice_type(SliceType::P)
            .frame_num(2)
            .pic_order_cnt_lsb(4)
            .build();
        mmco.dec_ref_pic_marking.adaptive_ref_pic_marking_mode_flag = true;
        mmco.dec_ref_pic_marking.inner.push(RefPicMarkingInner {
            memory_management_control_operation: 5,
            ..Default::default()
        });
        assert!(
            feed(
                &mut decoder,
                24,
                &encode_slice(&sps, &pps, &mmco, false, IsReference::ShortTerm)
            )
            .is_empty()
        );
        assert_eq!(
            feed(&mut decoder, 26, &slice(&sps, &pps, 1, 2, false)),
            [20, 22]
        );
        assert_eq!(
            observations.lock().unwrap().references.last().unwrap(),
            &[24]
        );
        assert!(feed(&mut decoder, 28, &slice(&sps, &pps, 2, 4, false)).is_empty());
        assert_eq!(
            feed(&mut decoder, 30, &slice(&sps, &pps, 3, 6, false)),
            [24]
        );
    }
}

#[test]
fn format_recovery_drains_old_pictures_and_applies_new_reorder_limit() {
    let observations = Arc::new(Mutex::new(Observations::default()));
    let mut decoder = StatelessDecoder::<H264, _>::new(
        TestBackend {
            info: None,
            observations: Arc::clone(&observations),
        },
        BlockingMode::NonBlocking,
    )
    .unwrap();
    let (sps, pps, parameters) = parameter_sets(2);
    feed(&mut decoder, 0, &parameters);
    feed(&mut decoder, 0, &slice(&sps, &pps, 0, 0, true));
    feed(&mut decoder, 6, &slice(&sps, &pps, 1, 6, false));
    feed(&mut decoder, 2, &slice(&sps, &pps, 2, 2, false));

    let (sps, pps, parameters) = encode_parameters(
        SpsBuilder::new()
            .resolution(128, 64)
            .frame_mbs_only_flag(true)
            .max_num_ref_frames(15)
            .max_dec_frame_buffering(15)
            .max_num_reorder_frames(0)
            .build(),
    );
    assert_eq!(feed(&mut decoder, 0, &parameters), [0, 2, 6]);
    assert_eq!(observations.lock().unwrap().resets, 1);
    assert!(feed(&mut decoder, 100, &slice(&sps, &pps, 0, 0, true)).is_empty());
    assert_eq!(
        feed(&mut decoder, 102, &slice(&sps, &pps, 1, 2, false)),
        [100]
    );
    assert_eq!(
        observations.lock().unwrap().references.last().unwrap(),
        &[100]
    );
}

#[test]
fn inferred_or_invalid_reorder_limits_do_not_become_zero_delay() {
    for restricted in [false, true] {
        let observations = Arc::new(Mutex::new(Observations::default()));
        let mut decoder = StatelessDecoder::<H264, _>::new(
            TestBackend {
                info: None,
                observations,
            },
            BlockingMode::NonBlocking,
        )
        .unwrap();
        let mut sps = SpsBuilder::new()
            .resolution(64, 64)
            .frame_mbs_only_flag(true)
            .max_num_ref_frames(15);
        if restricted {
            sps = sps.max_dec_frame_buffering(15).max_num_reorder_frames(16);
        }
        let (sps, pps, parameters) = encode_parameters(sps.build());
        feed(&mut decoder, 0, &parameters);
        for index in 0..5 {
            assert!(
                feed(
                    &mut decoder,
                    u64::from(index),
                    &slice(&sps, &pps, index, index * 2, index == 0)
                )
                .is_empty()
            );
        }
    }
}

#[test]
fn interlaced_frame_splitting_preserves_complete_frame_output() {
    let observations = Arc::new(Mutex::new(Observations::default()));
    let mut decoder = StatelessDecoder::<H264, _>::new(
        TestBackend {
            info: None,
            observations,
        },
        BlockingMode::NonBlocking,
    )
    .unwrap();
    let stream = include_bytes!(
        "../../../vendor/cros-codecs-extended/src/codec/h264/test_data/test-25fps-interlaced.h264"
    );
    let mut ready = feed(&mut decoder, 0, stream);
    decoder.flush().unwrap();
    while let Some(event) = decoder.next_event() {
        if let DecoderEvent::FrameReady(handle) = event {
            ready.push(handle.timestamp());
        }
    }
    let expected_frames = include_str!("../../../vendor/cros-codecs-extended/src/codec/h264/test_data/test-25fps-interlaced.h264.crc")
        .lines().count();
    assert_eq!(ready.len(), expected_frames);
}

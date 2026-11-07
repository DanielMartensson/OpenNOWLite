#![cfg(target_os = "linux")]

use std::path::PathBuf;
use std::time::{Duration, Instant};

use opennow_streamer_platform_linux::{
    BackendEvent, ColorPrimaries, ColorTransfer, DecodedVideoFrame, DecoderPreference,
    EncodedVideoFrame, LinuxSession, PixelFormat, SessionConfig, StreamFormat, VideoCodec,
};

#[test]
fn mediacodec_public_session_java_interop() {
    let Some(socket) = std::env::var_os("OPENNOW_MEDIACODEC_INTEROP_SOCKET") else {
        return;
    };
    let format = StreamFormat::video_default(64, 64).unwrap();
    let mut config = SessionConfig::new(format);
    config.audio = None;
    config.decoder_preference = DecoderPreference::MediaCodecOnly;
    config.mediacodec_socket = Some(PathBuf::from(socket));
    let mut session = LinuxSession::start(config).unwrap();
    let bitstream = include_bytes!("fixtures/software_decode_64x64.h264");
    let starts: Vec<_> = (0..bitstream.len() - 3)
        .filter(|index| bitstream[*index..*index + 3] == [0, 0, 1])
        .collect();
    let first_slice = starts
        .iter()
        .position(|index| bitstream[*index + 3] & 0x1f == 5)
        .unwrap();
    let access_unit = &bitstream[..starts[first_slice + 1]];
    for timestamp in [1234, 2234] {
        if timestamp == 2234 {
            session.reconfigure(format).unwrap();
            let deadline = Instant::now() + Duration::from_secs(3);
            loop {
                match session.try_recv_event() {
                    Some(BackendEvent::FormatChanged(changed)) => {
                        assert_eq!(changed, format);
                        break;
                    }
                    Some(BackendEvent::Error(error)) => panic!("reconfigure failed: {error}"),
                    _ => {}
                }
                assert!(Instant::now() < deadline, "reconfigure never completed");
                std::thread::sleep(Duration::from_millis(1));
            }
        }
        session
            .submit_video(EncodedVideoFrame::new(access_unit, timestamp, true).unwrap())
            .unwrap();
        let frame = session
            .recv_frame_timeout(Duration::from_secs(3))
            .expect("delayed Java fixture output");
        assert_pattern(&frame, format, timestamp);
    }
    session.stop().unwrap();
}

fn assert_pattern(frame: &DecodedVideoFrame, format: StreamFormat, timestamp: u64) {
    assert_eq!(frame.timestamp_us, timestamp);
    assert_eq!(frame.format, format);
    assert_eq!(frame.planes.len(), 2);
    assert_eq!(frame.planes[0].data.len(), 64 * 64);
    assert_eq!(frame.planes[1].data.len(), 64 * 32);
    for (index, value) in frame.planes[0].data.iter().enumerate() {
        assert_eq!(*value, (timestamp + index as u64) as u8);
    }
    for (index, value) in frame.planes[1].data.iter().enumerate() {
        assert_eq!(*value, (128 + index) as u8);
    }
}

#[test]
fn mediacodec_public_session_rejects_h265_and_hdr_before_connecting() {
    let sdr = StreamFormat::video_default(64, 64).unwrap();
    let hdr = StreamFormat {
        pixel_format: PixelFormat::P010,
        color_transfer: ColorTransfer::Pq,
        color_primaries: ColorPrimaries::Bt2020,
        ..sdr
    };
    for (codec, format) in [(VideoCodec::H265, sdr), (VideoCodec::H264, hdr)] {
        let mut config = SessionConfig::new(format);
        config.codec = codec;
        config.audio = None;
        config.decoder_preference = DecoderPreference::MediaCodecOnly;
        config.mediacodec_socket = Some(PathBuf::from("/not-an-opennow-mediacodec-socket"));
        let error = match LinuxSession::start(config) {
            Ok(_) => panic!("unsupported MediaCodec format opened"),
            Err(error) => error,
        };
        assert!(
            error
                .to_string()
                .contains("bridge requires H.264 NV12 SDR BT.709"),
            "unexpected error: {error}"
        );
    }
}

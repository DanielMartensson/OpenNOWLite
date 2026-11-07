#[cfg(target_os = "linux")]
use std::{
    path::PathBuf,
    sync::Arc,
    time::{Duration, Instant},
};

#[cfg(target_os = "linux")]
use anyhow::{Context, Result, ensure};
#[cfg(target_os = "linux")]
use opennow_streamer_platform_linux::{
    BackendEvent, DecoderBackend, DecoderPreference, EncodedVideoFrame, LinuxSession, PixelFormat,
    SessionConfig, StreamFormat,
};

#[cfg(target_os = "linux")]
fn main() -> Result<()> {
    let mut arguments = std::env::args_os().skip(1);
    let socket = PathBuf::from(
        arguments
            .next()
            .context("expected SOCKET BLACK_720P_H264")?,
    );
    let bitstream = std::fs::read(arguments.next().context("expected BLACK_720P_H264")?)?;
    ensure!(arguments.next().is_none(), "unexpected extra arguments");
    ensure!(
        bitstream.len() <= 16 * 1024 * 1024,
        "access unit is too large"
    );
    let mut config = SessionConfig::new(StreamFormat::h264_default(1280, 720)?);
    config.decoder_preference = DecoderPreference::MediaCodecOnly;
    config.mediacodec_socket = Some(socket);
    config.embedded_presentation = true;
    config.audio = None;
    for timestamp in [987_654_321, 987_687_654] {
        let mut session = LinuxSession::start(config.clone())?;
        let mut selected = false;
        while let Some(event) = session.try_recv_event() {
            if let BackendEvent::DecoderSelected(backend) = event {
                ensure!(
                    backend == DecoderBackend::MediaCodec,
                    "wrong decoder selected"
                );
                selected = true;
            }
        }
        ensure!(selected, "hardware bridge did not report decoder readiness");
        let started = Instant::now();
        session.submit_video(EncodedVideoFrame::new(
            Arc::<[u8]>::from(bitstream.as_slice()),
            timestamp,
            true,
        )?)?;
        let frame = session
            .recv_frame_timeout(Duration::from_secs(3))
            .with_context(|| format!("no decoded frame, state {:?}", session.state()))?;
        frame.validate()?;
        ensure!(frame.timestamp_us == timestamp, "sender timestamp changed");
        ensure!(
            frame.format == config.stream_format,
            "decoded format changed"
        );
        ensure!(frame.format.pixel_format == PixelFormat::Nv12, "not NV12");
        ensure!(
            frame.dmabuf.is_none() && frame.vulkan.is_none(),
            "unexpected output mode"
        );
        ensure!(frame.planes.len() == 2, "not two NV12 planes");
        for (plane, expected, columns, rows) in [
            (&frame.planes[0], 16_u8, 1280_usize, 720_usize),
            (&frame.planes[1], 128_u8, 1280_usize, 360_usize),
        ] {
            for row in [0, rows / 2, rows - 1] {
                for column in [0, columns / 2, columns - 1] {
                    ensure!(
                        plane.data[row * plane.stride + column].abs_diff(expected) <= 2,
                        "black-frame pixels are incorrect"
                    );
                }
            }
        }
        session.stop()?;
        println!(
            "MediaCodec copied-NV12 smoke passed: timestamp_us={timestamp}, elapsed_ms={}, pixels=black, close=acknowledged",
            started.elapsed().as_millis()
        );
    }
    Ok(())
}

#[cfg(not(target_os = "linux"))]
fn main() {
    eprintln!("The MediaCodec bridge smoke check requires Linux.");
    std::process::exit(2);
}

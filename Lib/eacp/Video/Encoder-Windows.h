#pragma once

#include <eacp/Core/Utils/WinInclude.h>

#include "Encoder.h"

#include <mfidl.h>
#include <mfreadwrite.h>
#include <wrl/client.h>

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <optional>

// Windows-only header, shared by Encoder-Windows.cpp and ScreenCapture-Windows.
// The Screen tier reads Windows.Graphics.Capture frames back as BGRA rows and
// hands them straight to the sink writer, so it needs the concrete
// WindowsEncoder's appendBGRA(), not just the portable Encoder interface --
// the same split Encoder-Apple.h makes for the ScreenCaptureKit path.

namespace eacp::Video
{

// Media Foundation IMFSinkWriter writing H.264 into an .mp4, with an AAC stream
// beside it when the spec asks for audio. Frames arrive as BGRA (RGB32) samples
// with a real-time presentation timestamp; the SinkWriter's implicit converter
// feeds the H.264 encoder, so playback runs at capture speed. The GpuDirect tier
// is not wired here yet (no D3D->MF zero-copy), so those hooks keep the base's
// "unsupported" default and callers fall back to Snapshot.
struct WindowsEncoder final : Encoder
{
    ~WindowsEncoder() override;

    bool begin(const FilePath& path, const EncoderSpec& spec) override;
    void appendImage(const Graphics::Image& image, double ptsSeconds) override;
    void appendAudio(const AudioBuffer& buffer, double ptsSeconds) override;
    bool acceptsAudio() const override { return writer && audioSpec.has_value(); }
    Threads::Async<void> finish() override;

    // Windows-only, used by the Screen tier: one already-composited BGRA frame
    // whose rows sit `stride` bytes apart (a mapped GPU surface pads them).
    // A source bigger than the stream begin() opened is cropped and a smaller
    // one is padded with black, so a window resized mid-recording keeps feeding
    // the file it opened rather than ending the recording.
    void appendBGRA(const std::uint8_t* rows,
                    int sourceWidth,
                    int sourceHeight,
                    std::size_t stride,
                    double ptsSeconds);

    bool configureStreams(const EncoderSpec& spec);
    bool configureAudioStream(const AudioSpec& spec);

    Microsoft::WRL::ComPtr<IMFSinkWriter> writer;
    DWORD streamIndex = 0;
    DWORD audioStreamIndex = 0;
    std::optional<AudioSpec> audioSpec;

    // Frames arrive on the capture thread and samples on the recorder's drain;
    // the sink writer takes one call at a time.
    std::mutex writeMutex;

    int width = 0;
    int height = 0;
    int fps = 60;
    bool comInitialized = false;
    bool mfStarted = false;

private:
    // Timestamps `buffer` as one video frame and writes it under writeMutex.
    void writeVideoBuffer(IMFMediaBuffer* buffer, double ptsSeconds);
};

} // namespace eacp::Video

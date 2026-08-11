#pragma once

#include "VideoRecorder.h"

namespace eacp::Graphics
{
class View;
}

namespace eacp::Video
{

struct Encoder;

// Taps the system compositor for a view's host window (ScreenCaptureKit on
// macOS, Windows.Graphics.Capture on Windows) and feeds the live composited
// window to the encoder in real time.
struct ScreenCapture
{
    virtual ~ScreenCapture() = default;

    // `encoder` must outlive this ScreenCapture and is opened lazily once the
    // window size is known. False when capture is unavailable: no host window,
    // missing permission, or an unsupported platform.
    virtual bool start(Graphics::View& view,
                       const FilePath& path,
                       const VideoOptions& options,
                       Encoder& encoder) = 0;

    // Resolves on the main thread once the file is fully written; keep the
    // recorder alive until it does.
    virtual Threads::Async<void> stop() = 0;
};

OwningPointer<ScreenCapture> makeScreenCapture();

} // namespace eacp::Video

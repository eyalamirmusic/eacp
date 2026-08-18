#pragma once

#include "VideoRecorder.h"

namespace eacp::Graphics
{
class View;
}

namespace eacp::Video
{

struct Encoder;

// The Screen capture tier: taps the system compositor for a view's host window
// (ScreenCaptureKit on macOS, Windows.Graphics.Capture on Windows) and feeds the
// live composited window -- 2D, GPU and WebView together -- to the encoder in
// real time. Platform-specific, and the one tier that can fail for reasons
// outside the caller's control (see start()), so callers keep another tier to
// fall back to.
struct ScreenCapture
{
    virtual ~ScreenCapture() = default;

    // Begins capturing `view`'s host window into `path`, feeding `encoder` (which
    // it opens once the window size is known -- lazily on macOS, where finding
    // the window is asynchronous). Returns false when screen capture is
    // unavailable here: no host window, a minimized one, missing permission, or
    // an OS without the API. `encoder` must outlive this ScreenCapture.
    virtual bool start(Graphics::View& view,
                       const FilePath& path,
                       const RecordingOptions& options,
                       Encoder& encoder) = 0;

    // Stops capturing and finalizes the file. Resolves on the main thread once
    // the file is fully written. Keep the recorder alive until it resolves.
    virtual Threads::Async<void> stop() = 0;
};

// Builds the platform screen-capture backend (ScreenCaptureKit / WGC).
OwningPointer<ScreenCapture> makeScreenCapture();

} // namespace eacp::Video

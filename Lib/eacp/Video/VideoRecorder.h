#pragma once

#include <eacp/Core/Core.h>
#include <eacp/Core/Threads/Async.h>

namespace eacp::Graphics
{
class View;
}

namespace eacp::Video
{

enum class CaptureMode
{
    // Off-screen compositing via View::renderToImage: captures any content
    // (paint, layers, GPU, and -- through the async path -- WebView), works
    // headless with no permission, but re-composites every frame on the CPU so
    // it is not meant for real-time heavy-GPU capture.
    Snapshot,

    // Taps the system compositor for this view's host window (ScreenCaptureKit on
    // macOS, Windows.Graphics.Capture on Windows): the live composited window --
    // 2D, GPU and WebView together -- delivered GPU-side, in real time. Requires
    // the window to be on-screen, plus Screen Recording permission on macOS.
    Screen,

    // Renders a GPUView straight into an IOSurface-backed CVPixelBuffer (shared
    // GPU memory) and hands it to the encoder -- no GPU->CPU read-back. Real-time,
    // off-screen, no permission, but GPU content only (start() fails if the view
    // has no native GPU content). For a GPUView; not for 2D/paint/WebView.
    GpuDirect,
};

struct VideoOptions
{
    // How frames are captured. Snapshot is the portable, permission-free default;
    // Screen is the real-time full-composite path (see CaptureMode).
    CaptureMode mode = CaptureMode::Snapshot;

    // Pixels per point. 0 uses the view's backing scale, exactly as
    // View::renderToImage does.
    float scale = 0.0f;

    // Target frames per second. Frames arriving faster than this (a 120 Hz
    // display, say) are dropped to hold the rate; presentation timestamps use
    // real elapsed time, so playback speed stays correct. 0 captures at the
    // display's refresh rate.
    int fps = 60;

    // Average H.264 bitrate in bits per second. 0 picks a size-based default.
    int bitrate = 0;
};

// Records a View to an H.264 video (.mov / .mp4, chosen by the path extension)
// by snapshotting it every display refresh with View::renderToImage and encoding
// the frames with real-time presentation timestamps, so playback runs at the
// speed it was captured. start(), the per-frame capture, and stop() all run on
// the main thread; the file is finalized asynchronously.
//
// A first cut built on the snapshot mechanism: well-suited to 2D / layer /
// moderate-GPU views. Each frame does a full off-screen recomposite (and, for a
// GPUView, a GPU read-back), so it is not intended for real-time capture of
// heavy GPU content. Apple (AVFoundation) only for now.
class VideoRecorder
{
public:
    VideoRecorder();
    ~VideoRecorder();

    // Begins recording `view` into `path`, overwriting any existing file.
    // Returns false if the writer could not be set up (unwritable path,
    // non-positive view size, or no available codec).
    bool start(Graphics::View& view,
               const FilePath& path,
               const VideoOptions& options = {});

    // Stops capturing and finalizes the file. The returned Async resolves on the
    // main thread once the file is fully written (immediately if not recording).
    // Keep this VideoRecorder alive until it resolves.
    Threads::Async<void> stop();

    bool isRecording() const;

private:
    struct Native;
    Pimpl<Native> impl;
};

} // namespace eacp::Video

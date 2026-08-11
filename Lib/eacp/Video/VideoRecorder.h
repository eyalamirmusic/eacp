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
    // Off-screen compositing via View::renderToImage. Captures any content and
    // needs no permission, but re-composites every frame on the CPU.
    Snapshot,

    // Taps the system compositor for this view's host window, GPU-side and in
    // real time. Needs the window on-screen, and permission on macOS.
    Screen,

    // Renders a GPUView straight into shared GPU memory, with no read-back.
    // start() fails when the view has no native GPU content.
    GpuDirect,
};

struct VideoOptions
{
    CaptureMode mode = CaptureMode::Snapshot;

    // Pixels per point; 0 uses the view's backing scale.
    float scale = 0.0f;

    // Frames arriving faster than this are dropped, but presentation timestamps
    // use real elapsed time so playback speed stays correct. 0 follows the
    // display's refresh rate.
    int fps = 60;

    // Average H.264 bitrate in bits per second. 0 picks a size-based default.
    int bitrate = 0;
};

// Records a View to H.264 (.mov / .mp4, chosen by the path extension) with
// real-time presentation timestamps. start(), the per-frame capture and stop()
// all run on the main thread; the file is finalized asynchronously.
class VideoRecorder
{
public:
    VideoRecorder();
    ~VideoRecorder();

    // Overwrites any existing file. False when the writer could not be set up:
    // unwritable path, non-positive view size, or no available codec.
    bool start(Graphics::View& view,
               const FilePath& path,
               const VideoOptions& options = {});

    // The Async resolves on the main thread once the file is fully written,
    // immediately if not recording. Keep this recorder alive until it does.
    Threads::Async<void> stop();

    bool isRecording() const;

private:
    struct Native;
    Pimpl<Native> impl;
};

} // namespace eacp::Video

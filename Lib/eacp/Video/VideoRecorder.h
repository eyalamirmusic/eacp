#pragma once

#include <eacp/Core/Core.h>
#include <eacp/Core/Threads/Async.h>

namespace eacp::Graphics
{
class View;
}

namespace eacp::Video
{

struct VideoOptions
{
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

#pragma once

#include "Audio.h"

#include <eacp/Core/Threads/Async.h>

#include <optional>

namespace eacp::Graphics
{
class View;
}

namespace eacp::Video
{

enum class CaptureMode
{
    // Off-screen compositing via View::renderToImage: paint, attached layers
    // and GPU content, headless and with no permission, but it re-composites
    // every frame on the CPU so it is not meant for real-time heavy-GPU
    // capture.
    //
    // An embedded WebView comes out BLANK. The page is only readable
    // asynchronously (View::renderToImageAsync, backed by the web runtime's own
    // snapshot call), which a per-frame capture has nowhere to wait for -- so a
    // view with web content in it wants the Screen tier.
    Snapshot,

    // Taps the system compositor for this view's host window (ScreenCaptureKit on
    // macOS, Windows.Graphics.Capture on Windows): the live composited window --
    // 2D, GPU and WebView together -- delivered GPU-side, in real time. Requires
    // the window to be on-screen, plus Screen Recording permission on macOS.
    //
    // What lands in the file is the whole window, title bar and all, since the
    // compositor knows nothing about which view was asked for.
    Screen,

    // Renders a GPUView straight into an IOSurface-backed CVPixelBuffer (shared
    // GPU memory) and hands it to the encoder -- no GPU->CPU read-back. Real-time,
    // off-screen, no permission, but GPU content only (start() fails if the view
    // has no native GPU content). For a GPUView; not for 2D/paint/WebView.
    GpuDirect,
};

// Whether this app may capture the screen, which the Screen tier needs and no
// other tier does. macOS exposes one bit here and no way to tell "never asked"
// from "refused"; Windows asks for no consent to capture the app's own window,
// so it is always true there.
bool hasScreenCapturePermission();

// Puts the system prompt up, which macOS does once per app and never again --
// after that only System Settings changes the answer. onResult lands on the
// main thread carrying the state as it stands then, which after a first-time
// grant is still false: macOS applies a new Screen Recording grant to the next
// launch, not to the running process. So a false here means "have the user
// grant it and relaunch" at least as often as it means no.
void requestScreenCapturePermission(std::function<void(bool)> onResult);

struct RecordingOptions
{
    // How frames are captured. Snapshot is the portable, permission-free default;
    // Screen is the real-time full-composite path (see CaptureMode).
    CaptureMode mode = CaptureMode::Snapshot;

    // Pixels per point. 0 uses the view's backing scale, exactly as
    // View::renderToImage does. Ignored by the Screen tier on Windows: the
    // compositor hands its frames over at the window's own pixel size.
    float scale = 0.0f;

    // Target frames per second. Frames arriving faster than this (a 120 Hz
    // display, say) are dropped to hold the rate; presentation timestamps use
    // real elapsed time, so playback speed stays correct. 0 captures at the
    // display's refresh rate.
    int fps = 60;

    // Average H.264 bitrate in bits per second. 0 picks a size-based default.
    int bitrate = 0;

    // Set to record sound as well, and push it with pushAudio(). There is no
    // audio source of eacp's own: the app owns the audio, so the app feeds it.
    std::optional<AudioSpec> audio;
};

// Records a View to an H.264 video (.mov / .mp4, chosen by the path extension),
// optionally with an AAC audio track the app pushes in.
//
// Every capture tier lays its frames on one timeline that starts when start()
// returns, and pushAudio anchors the audio to that same timeline, so a sound
// and the frame it belongs to stay together whichever tier is running.
//
// start(), the per-frame capture, and stop() all run on the main thread;
// pushAudio() is for the audio thread and the file is finalized asynchronously.
class VideoRecorder
{
public:
    VideoRecorder();
    ~VideoRecorder();

    // Begins recording `view` into `path`, overwriting any existing file.
    // Returns false if the writer could not be set up (unwritable path,
    // non-positive view size, no available codec, an audio spec the platform
    // encoder will not take, or the Screen tier without permission for it).
    bool start(Graphics::View& view,
               const FilePath& path,
               const RecordingOptions& options = {});

    // Copies one block into the lock-free ring the recorder drains: safe to
    // call from the audio thread, and a no-op unless recording with an audio
    // spec. A block that arrives faster than the encoder drains is dropped
    // whole and counted in droppedAudioFrames().
    void pushAudio(const AudioBuffer& buffer) noexcept;

    // Stops capturing and finalizes the file. The returned Async resolves on the
    // main thread once the file is fully written (immediately if not recording).
    // Keep this VideoRecorder alive until it resolves.
    Threads::Async<void> stop();

    bool isRecording() const;

    int droppedAudioFrames() const;

private:
    struct Native;
    Pimpl<Native> impl;
};

} // namespace eacp::Video

#pragma once

#include <eacp/Core/Core.h>

namespace eacp::Video
{
enum class PlayerState
{
    Idle, // nothing opened yet (or closed)
    Loading, // open() accepted the file; decoding not ready yet
    Ready, // dimensions/duration known, frames flowing on play()
    Failed // the file could not be opened or decoded
};

// A CPU-side copy of the latest decoded frame, tightly packed BGRA8
// (stride == width * 4). Used by the display path on backends without a
// zero-copy native buffer (Windows) and by pull-based consumers. sequence
// bumps once per new frame so a consumer can skip work when nothing changed.
// The Cameras::FramePixels of the playback path.
struct FramePixels
{
    int width = 0;
    int height = 0;
    Vector<std::uint8_t> data;
    std::uint64_t sequence = 0;
};

// Plays a video file (the formats the OS decodes natively: H.264/HEVC in
// .mp4/.mov and friends — AVFoundation on macOS, Media Foundation on Windows).
// Deliberately not a codec zoo: normal files play, exotic ones don't.
//
// Frame delivery mirrors Cameras::Camera, so the display path is the same
// shape for both: the player decodes on a thread of its own and publishes each
// frame as the latest, announcing it through setFrameArrivedCallback; the
// render path then takes it with acquireLatestPixelBuffer (zero-copy, macOS)
// or copyLatestFrame (CPU path). Nothing decodes on the render thread, and a
// view driven by arrivals redraws at the clip's frame rate rather than the
// display's.
//
// The playback clock, audio and A/V sync are the platform engine's (AVPlayer).
// Windows plays video only for now (no audio track output); playback is paced
// by sample timestamps on its decode thread.
class Player
{
public:
    Player();
    ~Player();

    Player(const Player&) = delete;
    Player& operator=(const Player&) = delete;

    // Starts loading the file asynchronously. Returns false only when the
    // backend refuses outright; load failures surface later through onError /
    // PlayerState::Failed. Ready is signalled through onReady.
    bool open(const FilePath& file);
    void close();

    void play();
    void pause();
    bool isPlaying() const;

    // Loop restarts playback at the end of the file; onEnded still fires at
    // every wrap.
    void setLooping(bool shouldLoop);
    bool isLooping() const;

    void setMuted(bool muted);
    void setVolume(float volume); // 0..1
    void setRate(double rate); // 1.0 = normal speed

    void seek(double seconds);

    PlayerState state() const;

    // 0 until the file is Ready.
    int width() const;
    int height() const;
    double duration() const;

    double currentTime() const;

    // Delivered on the main thread.
    std::function<void()> onReady = [] {};
    std::function<void()> onEnded = [] {};
    std::function<void(const std::string&)> onError = [](const std::string&) {};

    // Sets a lightweight notification invoked on the player's decode thread
    // after each decoded frame is stored as the latest — no pixel access; pair
    // it with acquireLatestPixelBuffer or copyLatestFrame. This may be
    // (re)assigned while the player runs. One consumer at a time: the display
    // path (VideoView) claims it while attached. Passing {} clears it.
    void setFrameArrivedCallback(Callback callback);

    // The most recent frame's native pixel buffer (CVPixelBufferRef on macOS),
    // retained — the caller owns the reference and must hand it back to
    // releasePixelBuffer. Null when no frame is available or the backend has
    // no zero-copy buffer (Windows), where copyLatestFrame is the path.
    // Thread-safe; the display path calls this on the render thread.
    void* acquireLatestPixelBuffer();

    // Releases a buffer returned by acquireLatestPixelBuffer.
    static void releasePixelBuffer(void* buffer);

    // Copies the latest frame into `out` when it is newer than out.sequence,
    // reusing out's storage; returns true if a new frame was copied.
    // Thread-safe.
    bool copyLatestFrame(FramePixels& out);

    // Bumps once per decoded frame published as the latest — driven by the
    // decode thread, not by the reader, so a consumer can compare it against
    // what it last drew and skip work when nothing changed.
    std::uint64_t frameSequence() const;

private:
    struct Native;
    Pimpl<Native> impl;
};
} // namespace eacp::Video

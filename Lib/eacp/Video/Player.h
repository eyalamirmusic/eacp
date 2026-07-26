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
struct PlayerFramePixels
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
// The playback clock, audio and A/V sync are the platform engine's
// (AVPlayer); frames are pulled on the render thread via
// acquireFramePixelBuffer() (zero-copy, macOS) or copyLatestFrame() (CPU
// path), mirroring Cameras::Camera. Drive the calls from the main thread;
// the frame pulls are safe from the render path.
//
// Windows plays video only for now (no audio track output); playback is paced
// by sample timestamps on a decode thread.
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

    // The most recent frame's native pixel buffer (CVPixelBufferRef on macOS),
    // retained — the caller owns the reference and must hand it back to
    // releasePixelBuffer. Null when no frame is available or the backend has
    // no zero-copy buffer (Windows), where copyLatestFrame is the path.
    // Pulling also advances the internal frame clock, so call once per render.
    void* acquireFramePixelBuffer();

    // Releases a buffer returned by acquireFramePixelBuffer.
    static void releasePixelBuffer(void* buffer);

    // Copies the latest frame into `out` when it is newer than out.sequence,
    // reusing out's storage; returns true if a new frame was copied.
    bool copyLatestFrame(PlayerFramePixels& out);

    // Bumps once per decoded frame that reached the display path. On macOS
    // frames are pulled, so this advances when acquireFramePixelBuffer /
    // copyLatestFrame run — poll it from a continuous render loop.
    std::uint64_t frameSequence() const;

private:
    struct Native;
    Pimpl<Native> impl;
};
} // namespace eacp::Video

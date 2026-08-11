#pragma once

#include "FrameStream.h"

namespace eacp::Video
{
// A playback clock over a FrameStream. Owns no timer and starts no thread: the
// caller advances it. Forward rates only.
class Player
{
public:
    explicit Player(FrameStream& streamToUse);

    void play();
    void pause();
    bool isPlaying() const { return playing; }

    // 1.0 is real time, 2.0 double speed, 0.5 half. Clamped to positive.
    void setRate(double rateToUse);
    double rate() const { return playbackRate; }

    void setLooping(bool shouldLoop);
    bool isLooping() const { return looping; }

    // Seeks the stream when the jump is backwards or far enough ahead that
    // decoding through the gap would cost more.
    void setPosition(double seconds);
    double position() const { return playhead; }

    // `delta` is real seconds, scaled by the rate. A no-op while paused.
    void advance(double delta);

    VideoFrame currentFrame();

    // True once playback has run past the end of a non-looping file.
    bool hasFinished() const { return finished; }

    FrameStream& stream() const { return source; }

private:
    double clampToDuration(double seconds) const;

    FrameStream& source;

    double playhead = 0.0;
    double playbackRate = 1.0;
    bool playing = false;
    bool looping = false;
    bool finished = false;
};
} // namespace eacp::Video

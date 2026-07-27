#pragma once

#include "FrameStream.h"

namespace eacp::Video
{
// A playback clock over a FrameStream: play/pause, rate, looping and a
// playhead, and nothing else. It owns no timer and starts no thread — the
// caller advances it:
//
//     player.advance(frameTime.delta);          // once per rendered frame
//     renderer.drawTexture(player.currentFrame(), ...);
//
// which is the whole point. A media player feeds it the display link's delta; a
// game feeds it the simulation's delta, so slow motion and pausing the world
// carry the video with them; an editor never calls advance() at all and drives
// setPosition() from the timeline playhead instead.
//
// Rates are forward-only for now: reverse playback needs the decoder to keep a
// decoded group of pictures around, which is a later phase behind this same
// interface.
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

    // Moves the playhead, seeking the stream when the jump is backwards or far
    // enough ahead that decoding through the gap would cost more than a seek.
    // The entry point for a scrub bar or an editor's playhead.
    void setPosition(double seconds);
    double position() const { return playhead; }

    // Advances the playhead by `delta` real seconds scaled by the rate. A no-op
    // while paused. At the end of the file this either stops or wraps around,
    // depending on isLooping().
    void advance(double delta);

    // The frame that belongs at the current playhead.
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

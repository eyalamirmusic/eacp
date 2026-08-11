#include "Player.h"

#include <algorithm>

namespace eacp::Video
{
namespace
{
// A seek throws away the queue and restarts the decoder, so a small nudge
// forward decodes through the gap instead.
constexpr auto decodeThroughSeconds = 0.5;
} // namespace

Player::Player(FrameStream& streamToUse)
    : source(streamToUse)
{
}

void Player::play()
{
    if (finished)
        setPosition(0.0);

    playing = true;
}

void Player::pause()
{
    playing = false;
}

void Player::setRate(double rateToUse)
{
    playbackRate = std::max(0.0, rateToUse);
}

void Player::setLooping(bool shouldLoop)
{
    looping = shouldLoop;
}

double Player::clampToDuration(double seconds) const
{
    auto duration = source.info().duration;
    auto clamped = std::max(0.0, seconds);

    return duration > 0.0 ? std::min(clamped, duration) : clamped;
}

void Player::setPosition(double seconds)
{
    auto target = clampToDuration(seconds);
    auto jumpedBack = target < playhead;
    auto jumpedFar = target > playhead + decodeThroughSeconds;

    playhead = target;
    finished = false;

    if (jumpedBack || jumpedFar)
        source.seek(target);
}

void Player::advance(double delta)
{
    if (!playing || delta <= 0.0)
        return;

    auto duration = source.info().duration;
    auto next = playhead + delta * playbackRate;

    if (duration > 0.0 && next >= duration)
    {
        if (looping)
        {
            setPosition(0.0);
            return;
        }

        playhead = duration;
        playing = false;
        finished = true;
        return;
    }

    playhead = next;

    // A file with no reported duration ends when the decoder runs dry.
    if (duration <= 0.0 && source.hasReachedEnd())
    {
        if (looping)
            setPosition(0.0);
        else
        {
            playing = false;
            finished = true;
        }
    }
}

VideoFrame Player::currentFrame()
{
    return source.frameAt(playhead);
}
} // namespace eacp::Video

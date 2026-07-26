#include "Common.h"

using namespace nano;
using namespace eacp;
using namespace VideoTests;

namespace
{
bool approx(double a, double b)
{
    return std::abs(a - b) < 0.0001;
}
} // namespace

// A paused player ignores advance(), which is what lets a game pause the world
// and have the video stop with it.
auto tPausedIgnoresAdvance = test("Player/pausedIgnoresAdvance") = []
{
    auto fake = FakeStream {30, 10.0};
    auto player = Video::Player {fake.stream};

    player.advance(1.0);
    check(approx(player.position(), 0.0));
    check(!player.isPlaying());
};

// Playing accumulates real time into the playhead.
auto tAdvanceAccumulates = test("Player/advanceAccumulates") = []
{
    auto fake = FakeStream {30, 10.0};
    auto player = Video::Player {fake.stream};

    player.play();
    player.advance(0.1);
    player.advance(0.1);

    check(approx(player.position(), 0.2));
};

// The rate scales the delta, so half speed covers half the ground per tick and
// double speed twice — the video's clock, not the display's, decides.
auto tRateScales = test("Player/rateScalesAdvance") = []
{
    auto fake = FakeStream {30, 10.0};
    auto player = Video::Player {fake.stream};

    player.play();
    player.setRate(0.5);
    player.advance(1.0);
    check(approx(player.position(), 0.5));

    player.setRate(2.0);
    player.advance(1.0);
    check(approx(player.position(), 2.5));
};

// Running off the end of a non-looping file stops playback at the duration.
auto tStopsAtEnd = test("Player/stopsAtEnd") = []
{
    auto fake = FakeStream {30, 10.0}; // three seconds
    auto player = Video::Player {fake.stream};

    player.play();
    player.advance(5.0);

    check(approx(player.position(), 3.0));
    check(!player.isPlaying());
    check(player.hasFinished());
};

// A looping file wraps to the start and keeps playing.
auto tLoops = test("Player/loopsAtEnd") = []
{
    auto fake = FakeStream {30, 10.0};
    auto player = Video::Player {fake.stream};

    player.setLooping(true);
    player.play();
    player.advance(5.0);

    check(approx(player.position(), 0.0));
    check(player.isPlaying());
    check(!player.hasFinished());
};

// Pressing play after the end restarts, which is what a play button is expected
// to do rather than sitting stuck on the last frame.
auto tPlayAfterEndRestarts = test("Player/playAfterEndRestarts") = []
{
    auto fake = FakeStream {30, 10.0};
    auto player = Video::Player {fake.stream};

    player.play();
    player.advance(5.0);
    check(player.hasFinished());

    player.play();
    check(approx(player.position(), 0.0));
    check(player.isPlaying());
};

// A small step forward is served by decoding through the gap; seeking would
// throw away a queue that already holds the answer.
auto tSmallStepForwardDoesNotSeek =
    test("Player/smallStepForwardDecodesThrough") = []
{
    auto fake = FakeStream {30, 10.0};
    auto player = Video::Player {fake.stream};

    player.setPosition(0.2);
    check(fake.decoder->seekCount.load() == 0);
};

// A backwards jump has to seek: the queue only ever holds frames ahead of the
// playhead. This is the scrub-bar path.
auto tBackwardsJumpSeeks = test("Player/backwardsJumpSeeks") = []
{
    auto fake = FakeStream {30, 10.0};
    auto player = Video::Player {fake.stream};

    player.setPosition(2.0);
    player.setPosition(0.5);

    // The seek is carried out on the decode thread, so the frame is what says
    // it happened; the counter is only readable once it has.
    check(indexOf(fake.stream.waitForFrameAt(0.55, waitTimeout)) == 5);
    check(fake.decoder->seekCount.load() >= 1);
};

// The playhead is clamped to the file, so a scrub past either end is harmless.
auto tPositionClamped = test("Player/positionClampedToFile") = []
{
    auto fake = FakeStream {30, 10.0};
    auto player = Video::Player {fake.stream};

    player.setPosition(-5.0);
    check(approx(player.position(), 0.0));

    player.setPosition(99.0);
    check(approx(player.position(), 3.0));
};

// The frame handed out is the one covering the playhead the player is on.
auto tCurrentFrameFollowsPlayhead = test("Player/currentFrameFollowsPlayhead") = []
{
    auto fake = FakeStream {30, 10.0};
    auto player = Video::Player {fake.stream};

    check(indexOf(fake.stream.waitForFrameAt(0.0, waitTimeout)) == 0);

    player.play();
    player.advance(0.25);

    check(indexOf(fake.stream.waitForFrameAt(player.position(), waitTimeout)) == 2);
    check(indexOf(player.currentFrame()) == 2);
};

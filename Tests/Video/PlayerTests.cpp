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

auto tPausedIgnoresAdvance = test("Player/pausedIgnoresAdvance") = []
{
    auto fake = FakeStream {30, 10.0};
    auto player = Video::Player {fake.stream};

    player.advance(1.0);
    check(approx(player.position(), 0.0));
    check(!player.isPlaying());
};

auto tAdvanceAccumulates = test("Player/advanceAccumulates") = []
{
    auto fake = FakeStream {30, 10.0};
    auto player = Video::Player {fake.stream};

    player.play();
    player.advance(0.1);
    player.advance(0.1);

    check(approx(player.position(), 0.2));
};

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

auto tSmallStepForwardDoesNotSeek =
    test("Player/smallStepForwardDecodesThrough") = []
{
    auto fake = FakeStream {30, 10.0};
    auto player = Video::Player {fake.stream};

    player.setPosition(0.2);
    check(fake.decoder->seekCount.load() == 0);
};

auto tBackwardsJumpSeeks = test("Player/backwardsJumpSeeks") = []
{
    auto fake = FakeStream {30, 10.0};
    auto player = Video::Player {fake.stream};

    player.setPosition(2.0);
    player.setPosition(0.5);

    // The seek runs on the decode thread, so the frame is what says it happened.
    check(indexOf(fake.stream.waitForFrameAt(0.55, waitTimeout)) == 5);
    check(fake.decoder->seekCount.load() >= 1);
};

auto tPositionClamped = test("Player/positionClampedToFile") = []
{
    auto fake = FakeStream {30, 10.0};
    auto player = Video::Player {fake.stream};

    player.setPosition(-5.0);
    check(approx(player.position(), 0.0));

    player.setPosition(99.0);
    check(approx(player.position(), 3.0));
};

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

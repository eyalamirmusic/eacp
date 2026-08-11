#include "Common.h"

using namespace nano;
using namespace eacp;
using namespace VideoTests;

auto tInfo = test("FrameStream/reportsTrackInfo") = []
{
    auto fake = FakeStream {30, 10.0};

    check(fake.stream.isOpen());
    check(fake.stream.info().width == 2);
    check(std::abs(fake.stream.info().duration - 3.0) < 0.001);
};

// Time 0.25 at 10fps is frame 2, which covers [0.2, 0.3).
auto tFrameForTime = test("FrameStream/framePicksIntervalContainingTime") = []
{
    auto fake = FakeStream {30, 10.0};

    check(indexOf(fake.stream.waitForFrameAt(0.0, waitTimeout)) == 0);
    check(indexOf(fake.stream.waitForFrameAt(0.05, waitTimeout)) == 0);
    check(indexOf(fake.stream.waitForFrameAt(0.25, waitTimeout)) == 2);
    check(indexOf(fake.stream.waitForFrameAt(0.9, waitTimeout)) == 9);
};

auto tSkipsStaleFrames = test("FrameStream/jumpForwardDropsPassedFrames") = []
{
    auto fake = FakeStream {30, 10.0};

    check(indexOf(fake.stream.waitForFrameAt(0.0, waitTimeout)) == 0);
    check(indexOf(fake.stream.waitForFrameAt(0.45, waitTimeout)) == 4);
};

auto tHoldsLastFrame = test("FrameStream/holdsLastFrameBetweenUpdates") = []
{
    auto fake = FakeStream {30, 10.0};

    check(indexOf(fake.stream.waitForFrameAt(0.25, waitTimeout)) == 2);

    check(indexOf(fake.stream.frameAt(0.26)) == 2);
    check(indexOf(fake.stream.frameAt(0.29)) == 2);
};

auto tSeek = test("FrameStream/seekRepositionsStream") = []
{
    auto fake = FakeStream {30, 10.0};

    check(indexOf(fake.stream.waitForFrameAt(0.25, waitTimeout)) == 2);

    fake.stream.seek(2.0);
    check(indexOf(fake.stream.waitForFrameAt(2.05, waitTimeout)) == 20);
    check(fake.decoder->seekCount.load() == 1);
};

auto tSeekBack = test("FrameStream/seekBackwards") = []
{
    auto fake = FakeStream {30, 10.0};

    check(indexOf(fake.stream.waitForFrameAt(2.05, waitTimeout)) == 20);

    fake.stream.seek(0.5);
    check(indexOf(fake.stream.waitForFrameAt(0.55, waitTimeout)) == 5);
};

auto tEndOfStream = test("FrameStream/reachesEndAfterQueueDrains") = []
{
    auto fake = FakeStream {5, 10.0};

    check(indexOf(fake.stream.waitForFrameAt(0.45, waitTimeout)) == 4);

    check(indexOf(fake.stream.waitForFrameAt(1.0, waitTimeout)) == 4);
    check(fake.stream.hasReachedEnd());
};

auto tQueueDepthBounds = test("FrameStream/queueDepthBoundsDecodeAhead") = []
{
    auto fake = FakeStream {1000, 10.0, 3};

    Time::sleepMS(200);

    // Three queued, plus at most one in flight when the wait took hold.
    check(fake.decoder->framesDecoded.load() <= 4);
};

auto tDepthBudget = test("FrameStream/queueDepthFitsMemoryBudget") = []
{
    auto options = Video::StreamOptions {};
    options.queueDepth = 4;
    options.maxQueueBytes = 192u * 1024u * 1024u;

    auto sized = [](int width, int height)
    {
        auto info = Video::VideoInfo {};
        info.width = width;
        info.height = height;
        return info;
    };

    // 1080p is 8 MB a frame, so all four fit with room to spare.
    check(Video::FrameStream::depthWithinBudget(sized(1920, 1080), options) == 4);

    // 4K is 33 MB; four still fit inside 192 MB.
    check(Video::FrameStream::depthWithinBudget(sized(3840, 2160), options) == 4);

    // 8K is 133 MB a frame — only one fits, and that is the floor.
    check(Video::FrameStream::depthWithinBudget(sized(7680, 4320), options) == 1);

    // The floor holds even when a single frame busts the budget outright.
    options.maxQueueBytes = 1024;
    check(Video::FrameStream::depthWithinBudget(sized(7680, 4320), options) == 1);

    options.maxQueueBytes = 192u * 1024u * 1024u;
    check(Video::FrameStream::depthWithinBudget(sized(0, 0), options) == 4);
};

auto tDefaultBudgetCoversEightK = test("FrameStream/defaultBudgetCoversEightK") = []
{
    auto options = Video::StreamOptions {};

    auto sized = [](int width, int height)
    {
        auto info = Video::VideoInfo {};
        info.width = width;
        info.height = height;
        return info;
    };

    // The two shapes 8K comes in: 16:9 at 133 MB a frame and DCI at 128 MB.
    check(Video::FrameStream::depthWithinBudget(sized(7680, 4320), options)
          == options.queueDepth);
    check(Video::FrameStream::depthWithinBudget(sized(8192, 4096), options)
          == options.queueDepth);
};

auto tReopen = test("FrameStream/closeThenReopen") = []
{
    auto fake = FakeStream {30, 10.0};

    check(indexOf(fake.stream.waitForFrameAt(1.05, waitTimeout)) == 10);

    fake.stream.close();
    check(!fake.stream.isOpen());

    auto options = Video::StreamOptions {};
    check(fake.stream.open(makeOwned<FakeDecoder>(30, 10.0), options));
    check(indexOf(fake.stream.waitForFrameAt(0.0, waitTimeout)) == 0);
};

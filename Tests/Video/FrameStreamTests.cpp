#include "Common.h"

using namespace nano;
using namespace eacp;
using namespace VideoTests;

// The stream reports what the decoder said about the track.
auto tInfo = test("FrameStream/reportsTrackInfo") = []
{
    auto fake = FakeStream {30, 10.0};

    check(fake.stream.isOpen());
    check(fake.stream.info().width == 2);
    check(std::abs(fake.stream.info().duration - 3.0) < 0.001);
};

// The playhead lands inside a frame's presentation interval, so time 0.25 at
// 10fps is frame 2 (which covers [0.2, 0.3)) rather than the nearest boundary.
auto tFrameForTime = test("FrameStream/framePicksIntervalContainingTime") = []
{
    auto fake = FakeStream {30, 10.0};

    check(indexOf(fake.stream.waitForFrameAt(0.0, waitTimeout)) == 0);
    check(indexOf(fake.stream.waitForFrameAt(0.05, waitTimeout)) == 0);
    check(indexOf(fake.stream.waitForFrameAt(0.25, waitTimeout)) == 2);
    check(indexOf(fake.stream.waitForFrameAt(0.9, waitTimeout)) == 9);
};

// A playhead that jumps forward drops the frames it passed instead of showing
// them: they are behind it and will never be seen. This is how a stream catches
// up after the consumer stalls.
auto tSkipsStaleFrames = test("FrameStream/jumpForwardDropsPassedFrames") = []
{
    auto fake = FakeStream {30, 10.0};

    check(indexOf(fake.stream.waitForFrameAt(0.0, waitTimeout)) == 0);
    check(indexOf(fake.stream.waitForFrameAt(0.45, waitTimeout)) == 4);
};

// Between decoded frames the last one handed out stays on screen, so a renderer
// always has something to draw.
auto tHoldsLastFrame = test("FrameStream/holdsLastFrameBetweenUpdates") = []
{
    auto fake = FakeStream {30, 10.0};

    check(indexOf(fake.stream.waitForFrameAt(0.25, waitTimeout)) == 2);

    // Still inside frame 2's interval — the same frame, and nothing new pulled.
    check(indexOf(fake.stream.frameAt(0.26)) == 2);
    check(indexOf(fake.stream.frameAt(0.29)) == 2);
};

// Seeking repositions the decoder and the frames that follow come from the new
// position.
auto tSeek = test("FrameStream/seekRepositionsStream") = []
{
    auto fake = FakeStream {30, 10.0};

    check(indexOf(fake.stream.waitForFrameAt(0.25, waitTimeout)) == 2);

    fake.stream.seek(2.0);
    check(indexOf(fake.stream.waitForFrameAt(2.05, waitTimeout)) == 20);
    check(fake.decoder->seekCount.load() == 1);
};

// Seeking backwards works the same way — the queue ahead of the playhead is
// thrown away rather than shown.
auto tSeekBack = test("FrameStream/seekBackwards") = []
{
    auto fake = FakeStream {30, 10.0};

    check(indexOf(fake.stream.waitForFrameAt(2.05, waitTimeout)) == 20);

    fake.stream.seek(0.5);
    check(indexOf(fake.stream.waitForFrameAt(0.55, waitTimeout)) == 5);
};

// The end of the file is reached only once the queue has drained too, and the
// last frame stays on screen rather than blinking out.
auto tEndOfStream = test("FrameStream/reachesEndAfterQueueDrains") = []
{
    auto fake = FakeStream {5, 10.0};

    check(indexOf(fake.stream.waitForFrameAt(0.45, waitTimeout)) == 4);

    // Past the last frame: it stays, and the stream reports the end.
    check(indexOf(fake.stream.waitForFrameAt(1.0, waitTimeout)) == 4);
    check(fake.stream.hasReachedEnd());
};

// The queue is the backpressure: a stream nobody draws decodes queueDepth
// frames and then stops, rather than running away with the whole file.
auto tQueueDepthBounds = test("FrameStream/queueDepthBoundsDecodeAhead") = []
{
    auto fake = FakeStream {1000, 10.0, 3};

    // Let the decode thread get as far as it is allowed to.
    Time::sleepMS(200);

    // Three queued, plus at most one in flight when the wait took hold.
    check(fake.decoder->framesDecoded.load() <= 4);
};

// A closed stream stops its decode thread and reports itself closed; reopening
// starts over from the beginning.
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

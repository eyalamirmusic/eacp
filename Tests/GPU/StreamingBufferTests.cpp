#include "Common.h"

// GPU::StreamingBuffers - the recycling itself, rather than anything it draws.
//
// The property that matters is a timing one: a buffer handed out again while
// the frame that used it may still be on the GPU is the tearing bug this type
// exists to prevent, and it is invisible in the pixels because it depends on
// how far behind the GPU happens to be. So none of this looks at an image. It
// checks which buffer comes back, and how many exist.
//
// Frames are advanced with Device::beginFrame(), which is exactly what Frame's
// constructor does - so this needs no window, no pass and no GPU device, and
// runs everywhere. The buffers themselves are invalid without a device, but
// they are still distinct objects at stable addresses, which is all the
// recycling is about.

using namespace nano;
using namespace eacp;
using namespace eacp::GPU;

namespace
{
constexpr auto framesInFlight = StreamingBuffers::framesInFlight;

// The payload is never read back, so its contents do not matter - but its
// length does: write() copies the byte count it is given, exactly as
// Buffer::update does, so a short source and a long count reads off the end.
const Buffer* writeOnce(StreamingBuffers& buffers, std::size_t bytes = 128)
{
    static auto payload = Vector<std::byte> {};

    if (payload.size() < (int) bytes)
        payload.resize((int) bytes);

    return &buffers.write(payload.data(), bytes);
}

// One frame, as far as anything streaming is concerned.
void nextFrame()
{
    Device::shared().beginFrame();
}
} // namespace

// The correctness property, stated directly: a buffer must not come back until
// framesInFlight frames have passed, because until then the frame that wrote it
// may still be on the GPU.
auto tRecyclesWithTheRightPeriod = test("StreamingBuffers/recyclesWithPeriod") = []
{
    auto buffers = StreamingBuffers {BufferUsage::Vertex};
    auto seen = Vector<const Buffer*> {};

    for (auto frame = 0; frame < 10; ++frame)
    {
        nextFrame();
        seen.add(writeOnce(buffers));
    }

    for (auto frame = 0; frame < seen.size(); ++frame)
    {
        // The same buffer comes back exactly one period later, and not before.
        if (frame + framesInFlight < seen.size())
            check(seen[frame] == seen[frame + framesInFlight]);

        for (auto other = frame + 1;
             other < seen.size() && other < frame + framesInFlight;
             ++other)
            check(seen[frame] != seen[other]);
    }
};

// The case a single rotating buffer gets wrong, and the reason this is a pool.
// A batching renderer flushes many times per frame through one shader, and the
// earlier flush's draw is still queued in the command buffer when the next one
// is written.
auto tSeveralWritesInOneFrame = test("StreamingBuffers/writesInOneFrameDiffer") = []
{
    auto buffers = StreamingBuffers {BufferUsage::Vertex};

    nextFrame();

    const auto* first = writeOnce(buffers);
    const auto* second = writeOnce(buffers);
    const auto* third = writeOnce(buffers);

    check(first != second);
    check(second != third);
    check(first != third);

    // And the next frame starts again at the first of them rather than carrying
    // on past three - otherwise a pool grows by one buffer every frame forever.
    nextFrame();
    nextFrame();
    nextFrame();

    check(writeOnce(buffers) == first);
};

// The whole point of the type. Steady drawing must stop allocating once the
// pools are warm, which is one buffer per frame in flight and no more.
auto tSteadyStateStopsAllocating = test("StreamingBuffers/steadyStateIsFlat") = []
{
    auto buffers = StreamingBuffers {BufferUsage::Vertex};

    for (auto frame = 0; frame < framesInFlight; ++frame)
    {
        nextFrame();
        writeOnce(buffers);
    }

    const auto warm = buffers.bufferCount();
    check(warm == framesInFlight);

    for (auto frame = 0; frame < 20; ++frame)
    {
        nextFrame();
        writeOnce(buffers);
    }

    check(buffers.bufferCount() == warm);
};

// Growing costs a GPU allocation, so it has to be geometric and it must never
// run backwards: a renderer whose load swings between frames would otherwise
// reallocate on most of them, which is the churn this replaced.
auto tGrowsAndNeverShrinks = test("StreamingBuffers/growsAndNeverShrinks") = []
{
    if (!Device::shared().isValid())
        return; // buffersCreated only counts buffers that got real storage

    auto buffers = StreamingBuffers {BufferUsage::Vertex};

    nextFrame();
    writeOnce(buffers, 4 * 1024 * 1024);

    const auto afterBigWrite = Device::shared().buffersCreated();
    const auto poolSize = buffers.bufferCount();

    // Back to the same pool, with a write that would fit in a far smaller
    // buffer. Reusing the big one is the point: shrinking to fit would mean an
    // allocation on the next large frame, and there is always a next one.
    for (auto frame = 0; frame < framesInFlight; ++frame)
        nextFrame();

    writeOnce(buffers, 64);

    check(Device::shared().buffersCreated() == afterBigWrite);
    check(buffers.bufferCount() == poolSize);
};

// Growth reaches the size asked for in one allocation rather than walking up to
// it through every doubling in between.
auto tFirstWriteAllocatesOnce = test("StreamingBuffers/firstWriteAllocatesOnce") = []
{
    if (!Device::shared().isValid())
        return;

    auto buffers = StreamingBuffers {BufferUsage::Vertex};

    const auto before = Device::shared().buffersCreated();

    nextFrame();
    writeOnce(buffers, 8 * 1024 * 1024);

    check(Device::shared().buffersCreated() == before + 1);
};

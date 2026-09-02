#include "Common.h"

#include <algorithm>

// GPU::StreamingBuffers - the recycling itself, rather than anything it draws.
//
// The property that matters is a timing one: storage handed out again while
// the frame that used it may still be on the GPU is the tearing bug this type
// exists to prevent, and it is invisible in the pixels because it depends on
// how far behind the GPU happens to be. So none of this looks at an image. It
// checks which arena a slice comes from, where in it, and how many exist.
//
// Frames are advanced with Device::beginFrame(), which is exactly what Frame's
// constructor does - so this needs no window, no pass and no GPU device, and
// runs everywhere. The arenas themselves are invalid without a device, but
// they are still distinct objects at stable addresses with the sizes they were
// asked for, which is all the recycling and the growth are about. Only the
// checks against Device::buffersCreated() need real storage, and they say so.
//
// StreamedRangeDrawTests is the other half: that a slice bound at its offset
// draws the geometry that was written there, on both backends.

using namespace nano;
using namespace eacp;
using namespace eacp::GPU;

namespace
{
constexpr auto framesInFlight = StreamingBuffers::framesInFlight;
constexpr auto alignment = StreamingBuffers::alignment;

// The payload is never read back, so its contents do not matter - but its
// length does: write() copies the byte count it is given, exactly as
// Buffer::update does, so a short source and a long count reads off the end.
BufferRange writeOnce(StreamingBuffers& buffers, std::size_t bytes = 128)
{
    static auto payload = Vector<std::byte> {};

    if (payload.size() < (int) bytes)
        payload.resize((int) bytes);

    return buffers.write(payload.data(), bytes);
}

// One frame, as far as anything streaming is concerned.
void nextFrame()
{
    Device::shared().beginFrame();
}

// One full period: the pool a write just went into comes round again.
void nextPeriod()
{
    for (auto frame = 0; frame < framesInFlight; ++frame)
        nextFrame();
}

bool overlaps(const BufferRange& a, const BufferRange& b)
{
    if (a.buffer != b.buffer)
        return false;

    return a.offset < b.offset + b.bytes && b.offset < a.offset + a.bytes;
}

// Every pair distinct, which for a frame's worth of slices is the property a
// draw depends on: none of them reads bytes another was given.
bool allDisjoint(Vector<BufferRange>& ranges)
{
    std::sort(ranges.begin(),
              ranges.end(),
              [](const BufferRange& a, const BufferRange& b)
              {
                  if (a.buffer != b.buffer)
                      return a.buffer < b.buffer;

                  return a.offset < b.offset;
              });

    for (auto i = 1; i < ranges.size(); ++i)
        if (overlaps(ranges[i - 1], ranges[i]))
            return false;

    return true;
}

bool isAligned(const BufferRange& range)
{
    return range.offset % alignment == 0;
}

bool hasDevice()
{
    return Device::shared().isValid();
}
} // namespace

// The correctness property, stated directly: an arena must not come back until
// framesInFlight frames have passed, because until then the frame that wrote
// it may still be on the GPU.
auto tRecyclesWithTheRightPeriod = test("StreamingBuffers/recyclesWithPeriod") = []
{
    auto buffers = StreamingBuffers {BufferUsage::Vertex};
    auto seen = Vector<BufferRange> {};

    for (auto frame = 0; frame < 10; ++frame)
    {
        nextFrame();
        seen.add(writeOnce(buffers));
    }

    for (auto frame = 0; frame < seen.size(); ++frame)
    {
        check(seen[frame].buffer != nullptr);

        // The same arena comes back exactly one period later, and not before.
        if (frame + framesInFlight < seen.size())
            check(seen[frame].buffer == seen[frame + framesInFlight].buffer);

        for (auto other = frame + 1;
             other < seen.size() && other < frame + framesInFlight;
             ++other)
            check(seen[frame].buffer != seen[other].buffer);
    }
};

// The case a single rotating buffer gets wrong, and the reason this is a pool.
// A batching renderer flushes many times per frame through one shader, and the
// earlier flush's draw is still queued in the command buffer when the next one
// is written. With an arena the flushes share one buffer and must not share
// any of its bytes.
auto tSeveralWritesInOneFrame = test("StreamingBuffers/writesInOneFrameAreDisjoint") = []
{
    auto buffers = StreamingBuffers {BufferUsage::Vertex};

    nextFrame();

    const auto first = writeOnce(buffers);
    const auto second = writeOnce(buffers);
    const auto third = writeOnce(buffers);

    // One arena, three slices of it, each after the last.
    check(first.buffer != nullptr);
    check(first.buffer == second.buffer);
    check(second.buffer == third.buffer);

    check(first.bytes == 128);
    check(first.offset < second.offset);
    check(second.offset < third.offset);

    check(!overlaps(first, second));
    check(!overlaps(second, third));
    check(!overlaps(first, third));

    check(isAligned(first));
    check(isAligned(second));
    check(isAligned(third));

    // And the pool's next frame starts again at the front rather than carrying
    // on past the three - otherwise an arena fills up by one slice every
    // period forever.
    nextPeriod();

    const auto again = writeOnce(buffers);

    check(again.buffer == first.buffer);
    check(again.offset == first.offset);
};

// The whole point of the type. Steady drawing must stop allocating once the
// pools are warm, which is one arena per frame in flight and no more.
auto tSteadyStateStopsAllocating = test("StreamingBuffers/steadyStateIsFlat") = []
{
    auto buffers = StreamingBuffers {BufferUsage::Vertex};

    for (auto frame = 0; frame < framesInFlight; ++frame)
    {
        nextFrame();
        writeOnce(buffers);
    }

    const auto warm = buffers.bufferCount();
    const auto reserved = buffers.bytesReserved();
    const auto created = Device::shared().buffersCreated();

    check(warm == framesInFlight);

    for (auto frame = 0; frame < 20; ++frame)
    {
        nextFrame();
        writeOnce(buffers);
    }

    check(buffers.bufferCount() == warm);
    check(buffers.bytesReserved() == reserved);
    check(Device::shared().buffersCreated() == created);
};

// Growing costs a GPU allocation, so it has to be geometric and it must never
// run backwards: a renderer whose load swings between frames would otherwise
// reallocate on most of them, which is the churn this replaced.
auto tGrowsAndNeverShrinks = test("StreamingBuffers/growsAndNeverShrinks") = []
{
    auto buffers = StreamingBuffers {BufferUsage::Vertex};

    nextFrame();
    writeOnce(buffers, 4 * 1024 * 1024);

    const auto afterBigWrite = Device::shared().buffersCreated();
    const auto poolSize = buffers.bufferCount();
    const auto reserved = buffers.bytesReserved();

    check(reserved >= 4 * 1024 * 1024);

    // Back to the same pool, with a write that would fit in a far smaller
    // arena. Reusing the big one is the point: shrinking to fit would mean an
    // allocation on the next large frame, and there is always a next one.
    nextPeriod();
    writeOnce(buffers, 64);

    check(Device::shared().buffersCreated() == afterBigWrite);
    check(buffers.bufferCount() == poolSize);
    check(buffers.bytesReserved() == reserved);
};

// Growth reaches the size asked for in one allocation rather than walking up to
// it through every doubling in between.
auto tFirstWriteAllocatesOnce = test("StreamingBuffers/firstWriteAllocatesOnce") = []
{
    if (!hasDevice())
        return; // buffersCreated only counts buffers that got real storage

    auto buffers = StreamingBuffers {BufferUsage::Vertex};

    const auto before = Device::shared().buffersCreated();

    nextFrame();
    writeOnce(buffers, 8 * 1024 * 1024);

    check(Device::shared().buffersCreated() == before + 1);
    check(buffers.bufferCount() == 1);
};

// A frame that outgrows its arena cannot have the arena replaced under the
// draws already recorded into it, so it gets another beside it - and the pool
// is back to one arena, big enough for that frame, the next time it comes
// round. Every slice handed out in the meantime is real and disjoint, which is
// what a draw recorded against one needs.
auto tOutgrownFrameFoldsBack = test("StreamingBuffers/outgrownFrameFoldsBack") = []
{
    auto buffers = StreamingBuffers {BufferUsage::Vertex};

    // Warm at the floor: three arenas, all small.
    for (auto frame = 0; frame < framesInFlight; ++frame)
    {
        nextFrame();
        writeOnce(buffers, 64);
    }

    const auto warm = buffers.bufferCount();
    check(warm == framesInFlight);

    // Then one frame that needs far more than the floor, in several pieces.
    nextFrame();

    auto ranges = Vector<BufferRange> {};

    for (auto piece = 0; piece < 3; ++piece)
        ranges.add(writeOnce(buffers, 100 * 1024));

    for (const auto& range: ranges)
    {
        check(range.buffer != nullptr);
        check(range.bytes == 100 * 1024);
        check(range.offset + range.bytes <= range.buffer->size());
        check(isAligned(range));
    }

    check(allDisjoint(ranges));
    check(buffers.bufferCount() > warm);

    // The pool comes round: one arena again, and one that holds the whole of
    // the frame that outgrew it. The fold itself may allocate that arena,
    // once, on the first write of this period - which is the last allocation
    // this pool makes for a frame of this size.
    nextPeriod();

    for (auto piece = 0; piece < 3; ++piece)
        writeOnce(buffers, 100 * 1024);

    check(buffers.bufferCount() == warm);

    // So the same frame once more, a period on, allocates nothing at all and
    // stays in the one arena from its first slice to its last.
    nextPeriod();

    const auto created = Device::shared().buffersCreated();
    const auto first = writeOnce(buffers, 100 * 1024);

    writeOnce(buffers, 100 * 1024);
    const auto last = writeOnce(buffers, 100 * 1024);

    check(buffers.bufferCount() == warm);
    check(last.buffer == first.buffer);
    check(Device::shared().buffersCreated() == created);
};

// The load this type was rebuilt for: a renderer that streams every draw's
// geometry, thousands of writes a frame of sizes that land in a different
// order every frame. One arena per pool absorbs it, and after the first period
// nothing is allocated however the order shuffles - where one buffer per write
// kept reallocating for as long as the order kept changing.
auto tManyShuffledWritesAllocateNothing =
    test("StreamingBuffers/manyShuffledWritesAllocateNothing") = []
{
    auto buffers = StreamingBuffers {BufferUsage::Vertex};

    // Two thousand sizes between 64 bytes and 8 KB, fixed by a small linear
    // congruential generator so the test is the same test every run.
    constexpr auto writesPerFrame = 2000;
    auto sizes = Vector<std::size_t> {};
    auto seed = std::uint32_t {12345};

    for (auto i = 0; i < writesPerFrame; ++i)
    {
        seed = seed * 1664525u + 1013904223u;
        sizes.add(64 + (seed >> 8) % (8 * 1024 - 64));
    }

    auto streamFrame = [&](int rotation)
    {
        nextFrame();

        auto ranges = Vector<BufferRange> {};

        for (auto i = 0; i < writesPerFrame; ++i)
            ranges.add(
                writeOnce(buffers, sizes[(i + rotation * 37) % writesPerFrame]));

        for (const auto& range: ranges)
        {
            check(range.buffer != nullptr);
            check(range.offset + range.bytes <= range.buffer->size());
            check(isAligned(range));
        }

        check(allDisjoint(ranges));
    };

    // The first period grows each pool to the frame; the fold after it may
    // allocate once more per pool. Two periods is what "warm" means here.
    for (auto frame = 0; frame < 2 * framesInFlight; ++frame)
        streamFrame(frame);

    const auto created = Device::shared().buffersCreated();
    const auto count = buffers.bufferCount();
    const auto reserved = buffers.bytesReserved();

    check(count == framesInFlight);

    for (auto frame = 0; frame < 12; ++frame)
        streamFrame(2 * framesInFlight + frame);

    check(Device::shared().buffersCreated() == created);
    check(buffers.bufferCount() == count);
    check(buffers.bytesReserved() == reserved);
};

// The other load that used to go wrong: one frame that never ends. A load
// screen redrawn thousands of times before its tick is over is thousands of
// writes to one pool, and one buffer per write made it thousands of GPU
// resources that nothing ever reclaimed. As slices it is bytes, held in a
// handful of arenas while the frame lasts and one afterwards.
auto tOneLongFrameGrowsBytesNotBuffers =
    test("StreamingBuffers/oneLongFrameGrowsBytesNotBuffers") = []
{
    auto buffers = StreamingBuffers {BufferUsage::Vertex};

    for (auto frame = 0; frame < framesInFlight; ++frame)
    {
        nextFrame();
        writeOnce(buffers, 64);
    }

    const auto warm = buffers.bufferCount();

    nextFrame();

    constexpr auto writes = 5000;
    constexpr auto bytesPerWrite = std::size_t {300};

    auto last = BufferRange {};

    for (auto i = 0; i < writes; ++i)
    {
        const auto range = writeOnce(buffers, bytesPerWrite);

        check(range.buffer != nullptr);
        check(range.offset + range.bytes <= range.buffer->size());

        if (last.buffer == range.buffer)
            check(range.offset >= last.offset + last.bytes);

        last = range;
    }

    // Doublings from the floor up to the frame, and no more: each arena
    // appended holds everything before it, so the walk is logarithmic in what
    // the frame streamed rather than linear in how often it wrote.
    check(buffers.bufferCount() <= warm + 8);
    check(buffers.bytesReserved() >= writes * alignment);

    nextPeriod();
    writeOnce(buffers, 64);

    check(buffers.bufferCount() == warm);
};

// Nothing to copy still names a real buffer, at the cursor, and takes no room:
// a program whose instance count fell to zero binds the arena rather than
// nothing, and the next write starts where this one did.
auto tZeroBytesTakesNoRoom = test("StreamingBuffers/zeroBytesTakesNoRoom") = []
{
    auto buffers = StreamingBuffers {BufferUsage::Vertex};

    nextFrame();

    const auto first = writeOnce(buffers, 64);
    const auto empty = buffers.write(nullptr, 0);
    const auto next = writeOnce(buffers, 64);

    check(empty.buffer == first.buffer);
    check(empty.bytes == 0);
    check(empty.offset == next.offset);
    check(next.offset > first.offset);
};

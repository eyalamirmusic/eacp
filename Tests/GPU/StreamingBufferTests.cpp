#include "Common.h"

// Frames are advanced with Device::beginFrame(), so these need no window, pass
// or GPU device: without one the buffers are invalid but still distinct objects
// at stable addresses, which is all the recycling is about.

using namespace nano;
using namespace eacp;
using namespace eacp::GPU;

namespace
{
constexpr auto framesInFlight = StreamingBuffers::framesInFlight;

// The contents do not matter, but the length does: write() copies the byte
// count it is given, so a short source and a long count reads off the end.
const Buffer* writeOnce(StreamingBuffers& buffers, std::size_t bytes = 128)
{
    static auto payload = Vector<std::byte> {};

    if (payload.size() < (int) bytes)
        payload.resize((int) bytes);

    return &buffers.write(payload.data(), bytes);
}

void nextFrame()
{
    Device::shared().beginFrame();
}
} // namespace

// Until framesInFlight frames have passed, the frame that wrote a buffer may
// still be on the GPU.
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
        if (frame + framesInFlight < seen.size())
            check(seen[frame] == seen[frame + framesInFlight]);

        for (auto other = frame + 1;
             other < seen.size() && other < frame + framesInFlight;
             ++other)
            check(seen[frame] != seen[other]);
    }
};

// A batching renderer flushes many times per frame through one shader, with the
// earlier flush's draw still queued when the next is written.
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

    // Otherwise the pool would grow by one buffer every frame forever.
    nextFrame();
    nextFrame();
    nextFrame();

    check(writeOnce(buffers) == first);
};

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

// Growing costs a GPU allocation, so it must never run backwards.
auto tGrowsAndNeverShrinks = test("StreamingBuffers/growsAndNeverShrinks") = []
{
    if (!Device::shared().isValid())
        return; // buffersCreated only counts buffers that got real storage

    auto buffers = StreamingBuffers {BufferUsage::Vertex};

    nextFrame();
    writeOnce(buffers, 4 * 1024 * 1024);

    const auto afterBigWrite = Device::shared().buffersCreated();
    const auto poolSize = buffers.bufferCount();

    // Shrinking to fit would mean an allocation on the next large frame.
    for (auto frame = 0; frame < framesInFlight; ++frame)
        nextFrame();

    writeOnce(buffers, 64);

    check(Device::shared().buffersCreated() == afterBigWrite);
    check(buffers.bufferCount() == poolSize);
};

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

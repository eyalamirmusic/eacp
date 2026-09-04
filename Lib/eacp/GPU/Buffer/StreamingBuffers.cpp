#include "StreamingBuffers.h"

#include "../Device/Device.h"

#include <algorithm>

// Entirely portable: N GPU::Buffers, a cursor, a growth rule and a fold. Both
// backends get the same recycling out of one implementation, which is what
// makes this a safe thing to add to a two-backend module.
//
// The arenas are BufferStorage::Streaming, which is the one thing here that
// reaches into a backend, and this type is the reason that storage can exist at
// all. A streamed write goes straight into memory the GPU may read without a
// copy to order it behind, so what makes it safe is that no arena is ever
// written while a frame that drew from it can still be in flight - the pool
// rotation below, and nothing else. On D3D12 that turns a write from a memcpy
// plus a barrier, a CopyBufferRegion and a barrier back into a memcpy; on Metal
// it is what a write already was.

namespace eacp::GPU
{
namespace
{
// A floor rather than fitting the first write exactly. What a renderer writes
// on its first frame says very little about what it settles at, and starting
// from one small allocation walks up through several doublings - each of which
// is a GPU allocation - before it gets there. Three pools of this is the whole
// cost of a stream that is barely used, which is why it is not larger.
constexpr auto minimumArenaBytes = std::size_t {64 * 1024};

// The cursor only ever advances by whole alignment units, so every slice
// starts where the next bind may begin.
std::size_t alignedUp(std::size_t bytes)
{
    constexpr auto mask = StreamingBuffers::alignment - 1;

    return (bytes + mask) & ~mask;
}

// Doubling rather than fitting each new high-water mark: what a batching
// renderer writes swings by a lot between frames, and resizing to every peak
// would allocate on most of them.
std::size_t grownCapacity(std::size_t needed, std::size_t current)
{
    auto capacity = std::max(current, minimumArenaBytes);

    while (capacity < needed)
        capacity *= 2;

    return capacity;
}
} // namespace

StreamingBuffers::StreamingBuffers(BufferUsage usageToUse)
    : usage(usageToUse)
{
}

// A frame other than the one that filled this pool has come round to it, which
// by construction is framesInFlight frames later - so nothing the GPU can still
// be reading lives in it, and the whole of it is free again.
void StreamingBuffers::beginFrame(Pool& pool, std::uint64_t frame)
{
    pool.frame = frame;
    pool.used = 0;
    pool.streamed = 0;

    if (pool.arenas.size() <= 1)
        return;

    // The frame before outgrew its arena and had more appended, which is the
    // one moment this pool holds more than one buffer. Folding them back into
    // one here, rather than the moment the frame outgrew it, is what keeps a
    // frame's draws pointing at storage that still exists; doing it at all is
    // what keeps steady state at one resource per pool.
    //
    // The arena appended last was sized for the whole of that frame up to the
    // write that made it, so more often than not it already holds everything
    // the frame streamed and the fold is simply dropping the ones before it.
    // When the frame carried on past it, one arena sized for the frame's high
    // water replaces them all - one allocation, and the last one this pool
    // makes for a frame of that size.
    if (pool.arenas.back()->size() >= pool.highWater)
    {
        auto kept = std::move(pool.arenas.back());

        pool.arenas.clear();
        pool.arenas.add(std::move(kept));

        return;
    }

    pool.arenas.clear();
    pool.arenas.createNew(Device::shared(),
                          nullptr,
                          grownCapacity(pool.highWater, 0),
                          usage,
                          BufferStorage::Streaming);
}

// The arena the next `bytes` go into: the current one when they fit after the
// cursor, otherwise a new one appended beside it.
Buffer& StreamingBuffers::arenaFor(Pool& pool, std::size_t bytes)
{
    if (!pool.arenas.empty())
    {
        auto& current = *pool.arenas.back();

        if (pool.used + bytes <= current.size())
            return current;
    }

    // Appended rather than replaced: the draws this frame has already recorded
    // hold ranges into the current arena, and a buffer under a recorded draw
    // has to stay where it is until the frame is off the GPU. Sized for
    // everything the frame has streamed so far plus this write, doubled from
    // the arena it outgrew, so that a frame which keeps growing walks up in a
    // handful of steps rather than one per write - and so that the fold at the
    // pool's next reset can usually keep this arena as the one that stays.
    const auto current =
        pool.arenas.empty() ? std::size_t {0} : pool.arenas.back()->size();

    pool.used = 0;

    return pool.arenas.createNew(Device::shared(),
                                 nullptr,
                                 grownCapacity(pool.streamed + bytes, current * 2),
                                 usage,
                                 BufferStorage::Streaming);
}

BufferRange StreamingBuffers::write(const void* data, std::size_t bytes)
{
    const auto frame = Device::shared().frameIndex();
    auto& pool = pools[(int) (frame % (std::uint64_t) framesInFlight)];

    if (pool.frame != frame)
        beginFrame(pool, frame);

    // Nothing to copy takes no room, but still names a real buffer: a caller
    // that binds whatever it last wrote - a program whose instance count went
    // to zero this frame - binds the arena at the cursor rather than nothing.
    if (data == nullptr || bytes == 0)
    {
        auto& arena = arenaFor(pool, 0);

        return {&arena, pool.used, 0};
    }

    auto& arena = arenaFor(pool, bytes);
    const auto offset = pool.used;

    arena.update(data, bytes, offset);

    const auto taken = alignedUp(bytes);

    pool.used += taken;
    pool.streamed += taken;
    pool.highWater = std::max(pool.highWater, pool.streamed);

    return {&arena, offset, bytes};
}

int StreamingBuffers::bufferCount() const
{
    auto total = 0;

    for (const auto& pool: pools)
        total += pool.arenas.size();

    return total;
}

std::size_t StreamingBuffers::bytesReserved() const
{
    auto total = std::size_t {0};

    for (const auto& pool: pools)
        for (const auto& arena: pool.arenas)
            total += arena->size();

    return total;
}
} // namespace eacp::GPU

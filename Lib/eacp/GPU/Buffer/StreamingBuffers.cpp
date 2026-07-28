#include "StreamingBuffers.h"

#include "../Device/Device.h"

#include <algorithm>

// Entirely portable: N GPU::Buffers, a growth rule and an index. Both backends
// get the same recycling out of one implementation, which is what makes this a
// safe thing to add to a two-backend module.

namespace eacp::GPU
{
namespace
{
// A floor rather than fitting the first write exactly. What a renderer writes
// on its first frame says very little about what it settles at, and starting
// from one small allocation walks up through several doublings - each of which
// is a GPU allocation - before it gets there.
constexpr auto minimumBufferBytes = std::size_t {16 * 1024};

// Doubling rather than fitting each new high-water mark: what a batching
// renderer writes swings by a lot between frames, and resizing to every peak
// would allocate on most of them.
std::size_t grownCapacity(std::size_t needed, std::size_t current)
{
    auto capacity = std::max(current, minimumBufferBytes);

    while (capacity < needed)
        capacity *= 2;

    return capacity;
}
} // namespace

StreamingBuffers::StreamingBuffers(BufferUsage usageToUse)
    : usage(usageToUse)
{
}

const Buffer& StreamingBuffers::write(const void* data, std::size_t bytes)
{
    auto& device = Device::shared();
    const auto frame = device.frameIndex();
    auto& pool = pools[(int) (frame % (std::uint64_t) framesInFlight)];

    // A frame other than the one that filled this pool has come round to it,
    // which by construction is framesInFlight frames later - so nothing the GPU
    // can still be reading lives in it and every buffer is free again.
    if (pool.frame != frame)
    {
        pool.frame = frame;
        pool.used = 0;
    }

    // Already grown on creation, so a first write of any size costs exactly one
    // allocation rather than one plus a doubling walk.
    if (pool.used == pool.buffers.size())
        pool.buffers.createNew(device, nullptr, grownCapacity(bytes, 0), usage);

    auto& slot = pool.buffers.get(pool.used);
    ++pool.used;

    // Grown in place rather than appended alongside: a pool's length is how
    // many writes a frame makes, not how many sizes it has asked for, and a
    // buffer big enough for this slot once stays big enough.
    if (slot->size() < bytes)
        slot.create(device, nullptr, grownCapacity(bytes, slot->size()), usage);

    slot->update(data, bytes);

    return *slot;
}

int StreamingBuffers::bufferCount() const
{
    auto total = 0;

    for (const auto& pool: pools)
        total += pool.buffers.size();

    return total;
}
} // namespace eacp::GPU

#include "StreamingBuffers.h"

#include "../Device/Device.h"

#include <algorithm>

namespace eacp::GPU
{
namespace
{
constexpr auto minimumBufferBytes = std::size_t {16 * 1024};

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

    // A different frame reached this pool, so it is framesInFlight frames old
    // and nothing the GPU can still be reading lives in it.
    if (pool.frame != frame)
    {
        pool.frame = frame;
        pool.used = 0;
    }

    if (pool.used == pool.buffers.size())
        pool.buffers.createNew(device, nullptr, grownCapacity(bytes, 0), usage);

    auto& slot = pool.buffers.get(pool.used);
    ++pool.used;

    // Grown in place: a pool's length tracks writes per frame, not sizes asked.
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

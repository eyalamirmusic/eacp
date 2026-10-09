#include "Buffer.h"

#include "BufferPool.h"

#include <utility>

namespace eacp::GPU
{
Buffer::~Buffer()
{
    giveBackToPool();
}

Buffer::Buffer(Buffer&& other) noexcept
    : impl(std::move(other.impl))
    , pool(std::exchange(other.pool, {}))
    , pooledUsage(other.pooledUsage)
{
}

Buffer& Buffer::operator=(Buffer&& other) noexcept
{
    if (this == &other)
        return *this;

    giveBackToPool();

    impl = std::move(other.impl);
    pool = std::exchange(other.pool, {});
    pooledUsage = other.pooledUsage;

    return *this;
}

void Buffer::giveBackToPool()
{
    auto link = std::exchange(pool, {});

    if (link.expired() || impl.get() == nullptr)
        return;

    auto key = BufferPool::Key {size(), pooledUsage};
    BufferPool::giveBack(link, std::move(*this), key);
}

bool Buffer::isPageAligned(const ExternalMemory& memory)
{
    const auto page = (std::uintptr_t) memoryPageSize();

    if (memory.bytes == nullptr || memory.byteCount <= 0 || page == 0)
        return false;

    return reinterpret_cast<std::uintptr_t>(memory.bytes) % page == 0;
}

BufferRange BufferRange::of(const Buffer& whole)
{
    return {&whole, 0, whole.size()};
}

bool BufferRange::isValid() const
{
    return buffer != nullptr && buffer->isValid();
}
} // namespace eacp::GPU

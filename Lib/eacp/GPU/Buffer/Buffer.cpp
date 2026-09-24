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
    , pool(std::exchange(other.pool, nullptr))
    , pooledUsage(other.pooledUsage)
{
}

Buffer& Buffer::operator=(Buffer&& other) noexcept
{
    if (this == &other)
        return *this;

    giveBackToPool();

    impl = std::move(other.impl);
    pool = std::exchange(other.pool, nullptr);
    pooledUsage = other.pooledUsage;

    return *this;
}

void Buffer::giveBackToPool()
{
    auto* owner = std::exchange(pool, nullptr);

    if (owner == nullptr || impl.get() == nullptr)
        return;

    auto key = BufferPool::Key {size(), pooledUsage};
    owner->give(std::move(*this), key);
}
} // namespace eacp::GPU

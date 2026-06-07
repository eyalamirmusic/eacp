#include "Buffer.h"

#include "../Device/Device.h"

// Windows/DirectX stub. See Device-Windows.cpp.

namespace eacp::GPU
{
struct Buffer::Native
{
    std::size_t length = 0;
};

Buffer::Buffer(Device&, const void*, std::size_t bytes)
    : impl()
{
    impl->length = bytes;
}

std::size_t Buffer::size() const
{
    return impl->length;
}

bool Buffer::isValid() const
{
    return false;
}

void* Buffer::nativeBuffer() const
{
    return nullptr;
}
} // namespace eacp::GPU

#include "Buffer.h"

namespace eacp::GPU
{
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

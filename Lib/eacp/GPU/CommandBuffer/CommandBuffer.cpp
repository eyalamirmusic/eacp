#include "CommandBuffer.h"

// Portable CommandBuffer members. The platform backends own recording,
// submission and the fence; what builds on the public API alone lives here so
// it compiles once for every platform.

namespace eacp::GPU
{
void CommandBuffer::update(Buffer& buffer,
                           const void* data,
                           std::int64_t bytes,
                           std::int64_t offset)
{
    // The whole of it: this command buffer's own wait, and then the write with
    // no second wait inside it. Buffer::update would order against the newest
    // submission, which is exactly what a caller reaching for this one has
    // already ordered against by hand and does not want to pay for again.
    wait();
    buffer.updateUnordered(data, bytes, offset);
}
} // namespace eacp::GPU

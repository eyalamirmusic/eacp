#include "Buffer.h"

#include "../Device/Device.h"
#include "../Linux/GPUBackend-Linux.h"

#include <unistd.h>

namespace eacp::GPU
{
struct Buffer::Native
{
    Native(Device& device,
           const void* data,
           std::int64_t bytes,
           BufferUsage usage,
           BufferStorage storage)
        : backend(getDeviceBackend(device)
                      .makeBuffer(device, data, bytes, usage, storage))
    {
    }

    std::unique_ptr<BufferBackend> backend;
};

Buffer::Buffer(Device& device,
               const void* data,
               std::int64_t bytes,
               BufferUsage usage,
               BufferStorage storage)
    : impl(device, data, bytes, usage, storage)
{
    // Only the ones that got storage, so the count means allocations.
    if (isValid())
        device.noteBufferCreated();
}

// Neither Linux backend can import host memory - Vulkan only where
// VK_EXT_external_memory_host is present and only in whole allocations of the
// device's own granularity, which is not something an API this shape can
// promise. So the memory is copied into a buffer of our own, the copy is taken
// by the constructor this delegates to, and the release runs the moment it
// returns - see ExternalMemory.
//
// A descriptor off the page grid is refused here as it is on Metal, so that a
// call site written against this backend is one Metal will also take.
Buffer::Buffer(Device& device, ExternalMemory memory, BufferUsage usage)
    : Buffer(device,
             isPageAligned(memory) ? memory.bytes : nullptr,
             isPageAligned(memory) ? memory.byteCount : 0,
             usage)
{
    memory.onReleased();
}

bool Buffer::canAdoptMemory(const Device&)
{
    return false;
}

std::int64_t Buffer::memoryPageSize()
{
    return (std::int64_t) sysconf(_SC_PAGESIZE);
}

std::int64_t Buffer::size() const
{
    return impl->backend->size();
}

bool Buffer::isValid() const
{
    return impl->backend->isValid();
}

void Buffer::read(void* dst, std::int64_t byteCount, std::int64_t byteOffset) const
{
    impl->backend->read(dst, byteCount, byteOffset);
}

void Buffer::update(const void* data,
                    std::int64_t byteCount,
                    std::int64_t byteOffset)
{
    impl->backend->update(data, byteCount, byteOffset);
}

void Buffer::updateUnordered(const void* data,
                             std::int64_t byteCount,
                             std::int64_t byteOffset)
{
    impl->backend->updateUnordered(data, byteCount, byteOffset);
}

void* Buffer::nativeBuffer() const
{
    return impl->backend->nativeBuffer();
}

void* Buffer::nativeReadView() const
{
    return impl->backend->nativeReadView();
}

void* Buffer::nativeWriteView() const
{
    return impl->backend->nativeWriteView();
}
} // namespace eacp::GPU

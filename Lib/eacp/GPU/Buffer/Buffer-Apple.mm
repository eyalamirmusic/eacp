#import <Metal/Metal.h>

#include "Buffer.h"

#include "../Device/Device.h"

#include <eacp/Core/ObjC/ObjC.h>

namespace eacp::GPU
{
struct Buffer::Native
{
    Native(Device& deviceToUse,
           const void* data,
           std::size_t bytes,
           BufferUsage,
           BufferStorage)
        : device(&deviceToUse)
        , length(bytes)
    {
        auto metalDevice = (__bridge id<MTLDevice>) device->nativeDevice();

        if (metalDevice == nil || bytes == 0)
            return;

        // Shared storage keeps the buffer CPU-visible, so read() is a memcpy and
        // a compute output needs no staging copy. The usage flag is a Metal no-op:
        // a plain MTLBuffer already serves as a vertex or a device storage buffer.
        //
        // So is BufferStorage: what it asks for on D3D12 - memory the CPU writes
        // in place and the GPU reads with no copy in between - is what every
        // buffer here already is, and update() below is already the memcpy it
        // buys. Metal has nothing to opt into and no second path to keep right.
        if (data != nullptr)
            buffer = [metalDevice newBufferWithBytes:data
                                              length:bytes
                                             options:MTLResourceStorageModeShared];
        else
            buffer = [metalDevice newBufferWithLength:bytes
                                              options:MTLResourceStorageModeShared];
    }

    ObjC::Ptr<NSObject<MTLBuffer>> buffer;
    Device* device = nullptr;
    std::size_t length = 0;
};

Buffer::Buffer(Device& device,
               const void* data,
               std::size_t bytes,
               BufferUsage usage,
               BufferStorage storage)
    : impl(device, data, bytes, usage, storage)
{
    // Only the ones that got storage, so the count means GPU allocations rather
    // than calls - a zero-byte or device-less Buffer allocated nothing.
    if (isValid())
        device.noteBufferCreated();
}

std::size_t Buffer::size() const
{
    return impl->length;
}

bool Buffer::isValid() const
{
    return impl->buffer.get() != nil;
}

void Buffer::read(void* dst, std::size_t bytes, std::size_t offset) const
{
    if (offset >= impl->length)
        return;

    // Shared storage makes the copy itself a memcpy, but the kernel that filled
    // the buffer may still be running: commitAsync returns before the GPU has
    // done anything at all. Waiting for the newest submission waits for every
    // earlier one too, which is the same ordering the D3D12 read gets from the
    // queue's fence — and costs nothing once the work has landed.
    if (impl->device != nullptr)
        impl->device->waitForSubmittedWork();

    auto available = impl->length - offset;
    auto count = bytes < available ? bytes : available;

    if (auto metalBuffer = impl->buffer.get())
        std::memcpy(dst, (const char*) [metalBuffer contents] + offset, count);
}

void Buffer::update(const void* data, std::size_t bytes, std::size_t offset)
{
    auto metalBuffer = impl->buffer.get();

    if (metalBuffer == nil || data == nullptr || bytes == 0
        || offset >= impl->length)
        return;

    auto available = impl->length - offset;
    auto count = bytes < available ? bytes : available;
    std::memcpy((char*) [metalBuffer contents] + offset, data, count);
}

void* Buffer::nativeBuffer() const
{
    return (__bridge void*) impl->buffer.get();
}

void* Buffer::nativeReadView() const
{
    return nullptr;
}

void* Buffer::nativeWriteView() const
{
    return nullptr;
}
} // namespace eacp::GPU

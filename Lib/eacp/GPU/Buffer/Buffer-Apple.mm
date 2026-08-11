#import <Metal/Metal.h>

#include "Buffer.h"

#include "../Device/Device.h"

#include <eacp/Core/ObjC/ObjC.h>

namespace eacp::GPU
{
struct Buffer::Native
{
    Native(Device& deviceToUse, const void* data, std::size_t bytes, BufferUsage)
        : device(&deviceToUse)
        , length(bytes)
    {
        auto metalDevice = (__bridge id<MTLDevice>) device->nativeDevice();

        if (metalDevice == nil || bytes == 0)
            return;

        // Shared storage keeps the buffer CPU-visible, so read() is a memcpy.
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

Buffer::Buffer(Device& device, const void* data, std::size_t bytes, BufferUsage usage)
    : impl(device, data, bytes, usage)
{
    // Counts GPU allocations, not calls: an invalid Buffer allocated nothing.
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

    // commitAsync returns before the GPU has run, so a kernel that fills this
    // buffer may still be in flight; the newest submission implies all earlier.
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

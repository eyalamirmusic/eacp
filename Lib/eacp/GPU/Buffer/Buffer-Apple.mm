#import <Metal/Metal.h>

#include "Buffer.h"

#include "../Device/Device.h"

#include <eacp/Core/ObjC/ObjC.h>

namespace eacp::GPU
{
struct Buffer::Native
{
    Native(Device& device, const void* data, std::size_t bytes, BufferUsage)
        : length(bytes)
    {
        auto metalDevice = (__bridge id<MTLDevice>) device.nativeDevice();

        if (metalDevice == nil || bytes == 0)
            return;

        // Shared storage keeps the buffer CPU-visible, so read() is a memcpy and
        // a compute output needs no staging copy. The usage flag is a Metal no-op:
        // a plain MTLBuffer already serves as a vertex or a device storage buffer.
        if (data != nullptr)
            buffer = [metalDevice newBufferWithBytes:data
                                              length:bytes
                                             options:MTLResourceStorageModeShared];
        else
            buffer = [metalDevice newBufferWithLength:bytes
                                              options:MTLResourceStorageModeShared];
    }

    ObjC::Ptr<NSObject<MTLBuffer>> buffer;
    std::size_t length = 0;
};

Buffer::Buffer(Device& device, const void* data, std::size_t bytes, BufferUsage usage)
    : impl(device, data, bytes, usage)
{
}

std::size_t Buffer::size() const
{
    return impl->length;
}

bool Buffer::isValid() const
{
    return impl->buffer.get() != nil;
}

void Buffer::read(void* dst, std::size_t bytes) const
{
    if (auto metalBuffer = impl->buffer.get())
        std::memcpy(dst, [metalBuffer contents], bytes);
}

void Buffer::update(const void* data, std::size_t bytes)
{
    auto metalBuffer = impl->buffer.get();

    if (metalBuffer == nil || data == nullptr || bytes == 0)
        return;

    auto count = bytes < impl->length ? bytes : impl->length;
    std::memcpy([metalBuffer contents], data, count);
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

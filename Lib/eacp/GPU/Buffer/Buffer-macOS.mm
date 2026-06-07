#import <Metal/Metal.h>

#include "Buffer.h"

#include "../Device/Device.h"

#include <eacp/Core/ObjC/ObjC.h>

namespace eacp::GPU
{
struct Buffer::Native
{
    Native(Device& device, const void* data, std::size_t bytes)
        : length(bytes)
    {
        auto metalDevice = (__bridge id<MTLDevice>) device.nativeDevice();

        if (metalDevice != nil && bytes > 0)
            buffer = [metalDevice newBufferWithBytes:data
                                              length:bytes
                                             options:MTLResourceStorageModeShared];
    }

    ObjC::Ptr<NSObject<MTLBuffer>> buffer;
    std::size_t length = 0;
};

Buffer::Buffer(Device& device, const void* data, std::size_t bytes)
    : impl(device, data, bytes)
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

void* Buffer::nativeBuffer() const
{
    return (__bridge void*) impl->buffer.get();
}
} // namespace eacp::GPU

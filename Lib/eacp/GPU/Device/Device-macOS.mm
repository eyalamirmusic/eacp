#import <Metal/Metal.h>

#include "Device.h"

#include <eacp/Core/ObjC/ObjC.h>

namespace eacp::GPU
{
struct Device::Native
{
    Native()
    {
        device = MTLCreateSystemDefaultDevice();

        if (device)
            queue = [device.get() newCommandQueue];
    }

    ObjC::Ptr<NSObject<MTLDevice>> device;
    ObjC::Ptr<NSObject<MTLCommandQueue>> queue;
};

Device::Device()
    : impl()
{
}

Device& Device::shared()
{
    static Device instance;
    return instance;
}

bool Device::isValid() const
{
    return impl->device.get() != nil;
}

void* Device::nativeDevice() const
{
    return (__bridge void*) impl->device.get();
}

void* Device::nativeQueue() const
{
    return (__bridge void*) impl->queue.get();
}
} // namespace eacp::GPU

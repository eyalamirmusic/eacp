#import <Metal/Metal.h>

#include "CommandBuffer.h"

#include "../Device/Device.h"

#include <eacp/Core/ObjC/ObjC.h>

namespace eacp::GPU
{
struct CommandBuffer::Native
{
    explicit Native(Device& device)
    {
        if (auto queue = (__bridge id<MTLCommandQueue>) device.nativeQueue())
            commandBuffer.reset((NSObject<MTLCommandBuffer>*) [queue commandBuffer]);
    }

    ObjC::Ptr<NSObject<MTLCommandBuffer>> commandBuffer;
};

CommandBuffer::CommandBuffer(Device& device)
    : impl(device)
{
}

ComputePass CommandBuffer::beginCompute()
{
    if (auto buffer = impl->commandBuffer.get())
        return ComputePass((__bridge void*) [buffer computeCommandEncoder]);

    return ComputePass(nullptr);
}

void CommandBuffer::commit()
{
    if (auto buffer = impl->commandBuffer.get())
    {
        [buffer commit];
        [buffer waitUntilCompleted];
    }
}

bool CommandBuffer::isValid() const
{
    return impl->commandBuffer.get() != nil;
}
} // namespace eacp::GPU

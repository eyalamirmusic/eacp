#import <Metal/Metal.h>

#include "CommandBuffer.h"

#include "../Device/Device.h"

#include <eacp/Core/ObjC/ObjC.h>
#include <eacp/Core/Threads/EventLoop.h>

namespace eacp::GPU
{
struct CommandBuffer::Native
{
    explicit Native(Device& deviceToUse)
        : device(&deviceToUse)
    {
        if (auto queue = (__bridge id<MTLCommandQueue>) device->nativeQueue())
            commandBuffer.reset((NSObject<MTLCommandBuffer>*) [queue commandBuffer]);
    }

    // Nil once already submitted, keeping a second commit a no-op rather than a
    // Metal assertion.
    id<MTLCommandBuffer> takeForCommit()
    {
        auto buffer = (id<MTLCommandBuffer>) commandBuffer.get();

        if (buffer == nil || committed)
            return nil;

        committed = true;
        device->trackSubmittedWork((__bridge void*) buffer);
        return buffer;
    }

    ObjC::Ptr<NSObject<MTLCommandBuffer>> commandBuffer;
    Device* device = nullptr;
    bool committed = false;
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
    if (auto buffer = impl->takeForCommit())
    {
        [buffer commit];
        [buffer waitUntilCompleted];
    }
}

Threads::Async<void> CommandBuffer::commitAsync()
{
    auto promise = Threads::AsyncPromise<void> {};
    auto buffer = impl->takeForCommit();

    if (buffer == nil)
    {
        promise.resolve();
        return promise.get();
    }

    // The completion handler runs on a Metal-owned thread, and an Async settles
    // on the main thread only — callAsync is the hop between the two.
    [buffer addCompletedHandler:^(id<MTLCommandBuffer>) {
        Threads::callAsync([promise] { promise.resolve(); });
    }];

    [buffer commit];

    return promise.get();
}

bool CommandBuffer::isValid() const
{
    return impl->commandBuffer.get() != nil;
}
} // namespace eacp::GPU

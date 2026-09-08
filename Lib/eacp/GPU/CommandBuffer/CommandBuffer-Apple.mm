#import <Metal/Metal.h>

#include "CommandBuffer.h"

#include "../Device/Device.h"
#include "../Timing/CommandTimer.h"

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

    // The buffer to submit, or nil once something already submitted it. Both
    // commit paths go through here, so the second call on one buffer is the
    // no-op the header promises rather than a Metal assertion.
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
    CommandTimer timer;
    bool committed = false;
};

CommandBuffer::CommandBuffer(Device& device)
    : impl(device)
{
}

ComputePass CommandBuffer::beginCompute(std::string_view label)
{
    auto buffer = (id<MTLCommandBuffer>) impl->commandBuffer.get();

    if (buffer == nil)
        return ComputePass(nullptr);

    auto passDescriptor = [MTLComputePassDescriptor computePassDescriptor];

    const auto pass =
        impl->timer.beginPass(label, *impl->device, (__bridge void*) buffer);

    if (pass >= 0)
    {
        if (auto samples =
                (__bridge id<MTLCounterSampleBuffer>) impl->timer.nativeSamples())
        {
            auto attachment = passDescriptor.sampleBufferAttachments[0];

            attachment.sampleBuffer = samples;
            attachment.startOfEncoderSampleIndex = (NSUInteger) (pass * 2);
            attachment.endOfEncoderSampleIndex = (NSUInteger) (pass * 2 + 1);
        }
    }

    return ComputePass((__bridge void*)
        [buffer computeCommandEncoderWithDescriptor:passDescriptor]);
}

void CommandBuffer::fill(const BufferRange& range, std::uint8_t value)
{
    auto buffer = (id<MTLCommandBuffer>) impl->commandBuffer.get();

    if (buffer == nil || !range.isValid() || range.bytes <= 0 || range.offset < 0
        || range.offset >= range.buffer->size())
        return;

    auto target = (__bridge id<MTLBuffer>) range.buffer->nativeBuffer();

    if (target == nil)
        return;

    const auto available = range.buffer->size() - range.offset;
    const auto length = range.bytes < available ? range.bytes : available;

    auto blit = [buffer blitCommandEncoder];

    [blit fillBuffer:target
               range:NSMakeRange((NSUInteger) range.offset, (NSUInteger) length)
               value:value];

    [blit endEncoding];
}

void CommandBuffer::commit()
{
    if (auto buffer = impl->takeForCommit())
    {
        // Before the commit: a committed buffer may finish at any moment.
        impl->timer.endRecording((__bridge void*) buffer);

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

    impl->timer.endRecording((__bridge void*) buffer);

    // The completion handler runs on a Metal-owned thread, and an Async settles
    // on the main thread only — callAsync is the hop between the two.
    [buffer addCompletedHandler:^(id<MTLCommandBuffer>) {
        Threads::callAsync([promise] { promise.resolve(); });
    }];

    [buffer commit];

    return promise.get();
}

const FrameTimings& CommandBuffer::timings()
{
    return impl->timer.timings(*impl->device);
}

bool CommandBuffer::supportsPassTimings() const
{
    return impl->timer.isSupported();
}

bool CommandBuffer::isValid() const
{
    return impl->commandBuffer.get() != nil;
}
} // namespace eacp::GPU

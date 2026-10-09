#import <Metal/Metal.h>

#include "CommandBuffer.h"

#include "../Device/Device.h"
#include "../Timing/CommandTimer.h"

#include <eacp/Core/ObjC/ObjC.h>
#include <eacp/Core/Threads/EventLoop.h>
#include <eacp/Core/Utils/Logging.h>

#include <cstring>

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

    // The buffer whose completion wait() and isComplete() ask about, the Ptr
    // above outliving the commit that handed it to the queue.
    id<MTLCommandBuffer> submitted() const
    {
        return committed ? (id<MTLCommandBuffer>) commandBuffer.get() : nil;
    }

    // A compute encoder on this command buffer, timed as a pass of its own
    // when it has a label and the device has counters to time it with.
    void* openEncoder(std::string_view label, DispatchOrder order)
    {
        auto buffer = (id<MTLCommandBuffer>) commandBuffer.get();
        auto passDescriptor = [MTLComputePassDescriptor computePassDescriptor];

        if (order == DispatchOrder::Concurrent)
            passDescriptor.dispatchType = MTLDispatchTypeConcurrent;

        const auto pass = timer.beginPass(label, *device, (__bridge void*) buffer);

        if (pass >= 0)
        {
            if (auto samples = (__bridge id<MTLCounterSampleBuffer>) timer.nativeSamples())
            {
                auto attachment = passDescriptor.sampleBufferAttachments[0];

                attachment.sampleBuffer = samples;
                attachment.startOfEncoderSampleIndex = (NSUInteger) (pass * 2);
                attachment.endOfEncoderSampleIndex = (NSUInteger) (pass * 2 + 1);
            }
        }

        return (__bridge void*) [buffer computeCommandEncoderWithDescriptor:passDescriptor];
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

ComputePass CommandBuffer::beginCompute(std::string_view label,
                                        DispatchOrder order,
                                        TimingScope scope)
{
    impl->device->assertOwningThread();

    if (impl->commandBuffer.get() == nil)
        return ComputePass(nullptr, order);

    if (scope == TimingScope::Pass)
        return ComputePass(impl->openEncoder(label, order), order);

    // Apple silicon samples its counters only where an encoder starts and ends,
    // so a dispatch timed on its own is an encoder of its own.
    auto* native = impl.get();

    return ComputePass(
        impl->openEncoder({}, order),
        order,
        [native, order](std::string_view dispatchLabel)
        { return native->openEncoder(dispatchLabel, order); },
        std::string {label});
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

void CommandBuffer::submit()
{
    impl->device->assertOwningThread();

    if (auto buffer = impl->takeForCommit())
    {
        // Before the commit: a committed buffer may finish at any moment.
        impl->timer.endRecording((__bridge void*) buffer);

        [buffer commit];
    }
}

void CommandBuffer::commit()
{
    submit();
    wait();
}

void CommandBuffer::wait()
{
    impl->device->assertOwningThread();

    // waitUntilCompleted on a buffer that already finished returns at once, so
    // a wait after the work has landed costs nothing.
    if (auto buffer = impl->submitted())
    {
        [buffer waitUntilCompleted];

        if (buffer.status == MTLCommandBufferStatusError && buffer.error != nil)
            LOG("GPU command buffer failed: ", [buffer.error.localizedDescription UTF8String]);
    }
}

bool CommandBuffer::isComplete() const
{
    auto buffer = impl->submitted();

    if (buffer == nil)
        return false;

    const auto status = buffer.status;

    return status == MTLCommandBufferStatusCompleted
           || status == MTLCommandBufferStatusError;
}

void CommandBuffer::read(const Buffer& buffer,
                         void* dst,
                         std::int64_t bytes,
                         std::int64_t offset)
{
    wait();

    if (dst == nullptr || bytes <= 0 || offset < 0 || offset >= buffer.size())
        return;

    auto target = (__bridge id<MTLBuffer>) buffer.nativeBuffer();

    if (target == nil)
        return;

    const auto available = buffer.size() - offset;
    const auto count = bytes < available ? bytes : available;

    std::memcpy(dst, (const char*) [target contents] + offset, (std::size_t) count);
}

Threads::Async<void> CommandBuffer::commitAsync()
{
    // The submission is the part that belongs to this thread; the completion
    // handler below hops to the message thread on its own and asserts nothing.
    impl->device->assertOwningThread();

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

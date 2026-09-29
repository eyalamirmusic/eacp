#include "CommandBuffer.h"

#include "../Device/Device.h"
#include "../Timing/CommandTimer.h"
#include "../WebGPU/WebGPUTypes.h"

#include <cstring>

namespace eacp::GPU
{
struct CommandBuffer::Native
{
    explicit Native(Device& deviceToUse)
        : device(&deviceToUse)
        , context(getWebGPUContext(deviceToUse))
    {
        encoder = context.beginRecording();
    }

    ~Native()
    {
        if (encoder != nullptr && !committed)
            context.discard(encoder);
    }

    bool canSubmit() const { return encoder != nullptr && !committed; }

    // The recording's timestamps are resolved onto it before it goes.
    void endAndSubmit()
    {
        committed = true;
        timer.endRecording(encoder);

        completionValue = context.submit(encoder);
        encoder = nullptr;

        timer.noteSubmitted(completionValue);
    }

    bool timePass(WGPUPassTimestampWrites& writes, std::string_view label)
    {
        const auto pass = timer.beginPass(label, *device, encoder);
        auto querySet = static_cast<WGPUQuerySet>(timer.nativeSamples());

        if (pass < 0 || querySet == nullptr)
            return false;

        writes.querySet = querySet;
        writes.beginningOfPassWriteIndex = static_cast<std::uint32_t>(pass * 2);
        writes.endOfPassWriteIndex = static_cast<std::uint32_t>(pass * 2 + 1);

        return true;
    }

    // ClearBuffer only writes zeros; any other byte is copied in from a buffer
    // filled at creation.
    void fillWith(const WebBufferData& data,
                  std::uint64_t offset,
                  std::uint64_t length,
                  std::uint8_t value)
    {
        if (value == 0)
        {
            wgpuCommandEncoderClearBuffer(encoder, data.buffer, offset, length);
            return;
        }

        auto descriptor = WGPU_BUFFER_DESCRIPTOR_INIT;
        descriptor.usage = WGPUBufferUsage_CopySrc;
        descriptor.size = length;
        descriptor.mappedAtCreation = true;

        auto pattern = wgpuDeviceCreateBuffer(context.getDevice(), &descriptor);

        if (pattern == nullptr)
            return;

        auto* mapped = wgpuBufferGetMappedRange(pattern, 0, WGPU_WHOLE_MAP_SIZE);

        if (mapped != nullptr)
            std::memset(mapped, value, static_cast<std::size_t>(length));

        wgpuBufferUnmap(pattern);
        wgpuCommandEncoderCopyBufferToBuffer(
            encoder, pattern, 0, data.buffer, offset, length);
        wgpuBufferRelease(pattern);
    }

    Device* device = nullptr;
    WebGPUContext& context;
    WGPUCommandEncoder encoder = nullptr;
    CommandTimer timer;
    std::uint64_t completionValue = 0;
    bool committed = false;
};

CommandBuffer::CommandBuffer(Device& device)
    : impl(device)
{
}

ComputePass CommandBuffer::beginCompute(std::string_view label, DispatchOrder order)
{
    impl->device->assertOwningThread();

    if (!impl->canSubmit())
        return ComputePass(nullptr, order);

    auto descriptor = WGPU_COMPUTE_PASS_DESCRIPTOR_INIT;
    descriptor.label = toWebString(label);

    auto timestamps = WGPU_PASS_TIMESTAMP_WRITES_INIT;

    if (impl->timePass(timestamps, label))
        descriptor.timestampWrites = &timestamps;

    auto* encoder = new WebComputeEncoder {};
    encoder->pass = wgpuCommandEncoderBeginComputePass(impl->encoder, &descriptor);
    encoder->context = &impl->context;

    return ComputePass(encoder, order);
}

void CommandBuffer::fill(const BufferRange& range, std::uint8_t value)
{
    if (!impl->canSubmit() || !range.isValid() || range.bytes <= 0
        || range.offset < 0 || range.offset >= range.buffer->size()
        || range.offset % 4 != 0)
        return;

    auto* data = static_cast<WebBufferData*>(range.buffer->nativeBuffer());

    if (data == nullptr || data->buffer == nullptr)
        return;

    const auto available = data->size - static_cast<std::size_t>(range.offset);
    const auto wanted = static_cast<std::size_t>(range.bytes);
    const auto length = (wanted < available ? wanted : available) & ~std::size_t {3};

    if (length == 0)
        return;

    impl->fillWith(*data,
                   static_cast<std::uint64_t>(range.offset),
                   static_cast<std::uint64_t>(length),
                   value);
}

void CommandBuffer::submit()
{
    impl->device->assertOwningThread();

    if (impl->canSubmit())
        impl->endAndSubmit();
}

// Submits and cannot wait: the queue finishes only once control is back with
// the browser. commitAsync is the call that sees the work done.
void CommandBuffer::commit()
{
    submit();
    wait();
}

Threads::Async<void> CommandBuffer::commitAsync()
{
    impl->device->assertOwningThread();

    auto promise = Threads::AsyncPromise<void> {};

    if (!impl->canSubmit())
    {
        promise.resolve();
        return promise.get();
    }

    impl->endAndSubmit();

    impl->context.notifyWhenCompleted(impl->completionValue,
                                      [promise] { promise.resolve(); });

    return promise.get();
}

void CommandBuffer::wait()
{
    impl->device->assertOwningThread();

    if (impl->committed && !impl->context.hasCompleted(impl->completionValue))
        reportWebUnsupported("CommandBuffer::wait");
}

bool CommandBuffer::isComplete() const
{
    return impl->committed && impl->context.hasCompleted(impl->completionValue);
}

void CommandBuffer::read(const Buffer& buffer,
                         void* dst,
                         std::int64_t bytes,
                         std::int64_t offset)
{
    wait();
    buffer.read(dst, bytes, offset);
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
    return impl->encoder != nullptr || impl->committed;
}
} // namespace eacp::GPU

#include "CommandBuffer.h"

#include "../Device/Device.h"
#include "../Timing/CommandTimer.h"
#include "../WebGPU/WebGPUContext.h"

#include <algorithm>
#include <memory>

namespace eacp::GPU
{
struct CommandBuffer::Native
{
    explicit Native(Device& deviceToUse)
        : device(&deviceToUse)
        , context(getWebGPUContext(deviceToUse))
        , recording(context.beginRecording())
    {
    }

    bool canRecord() const
    {
        return recording != nullptr && !committed && !recording->passOpen;
    }

    bool canSubmit() const { return recording != nullptr && !committed; }

    void endAndSubmit()
    {
        committed = true;
        context.submit(*recording);

        auto done = completed;
        context.getQueue().OnSubmittedWorkDone(
            wgpu::CallbackMode::AllowSpontaneous,
            [done](wgpu::QueueWorkDoneStatus, wgpu::StringView) { *done = true; });
    }

    Device* device = nullptr;
    WebGPUContext& context;
    OwningPointer<WebGPURecording> recording;
    std::shared_ptr<bool> completed = std::make_shared<bool>(false);
    CommandTimer timer;
    bool committed = false;
};

CommandBuffer::CommandBuffer(Device& device)
    : impl(device)
{
}

ComputePass CommandBuffer::beginCompute(std::string_view label, DispatchOrder order)
{
    impl->device->assertOwningThread();

    if (!impl->canRecord())
        return ComputePass(nullptr, order);

    auto descriptor = wgpu::ComputePassDescriptor {};
    descriptor.label = wgpu::StringView {label.data(), label.size()};

    auto* encoder = new WebGPUComputeEncoder {
        &impl->context,
        impl->recording.get(),
        impl->recording->encoder.BeginComputePass(&descriptor)};

    impl->recording->passOpen = true;
    return ComputePass(encoder, order);
}

void CommandBuffer::fill(const BufferRange& range, std::uint8_t value)
{
    if (!impl->canRecord() || !range.isValid() || range.bytes <= 0
        || range.offset < 0 || range.offset >= range.buffer->size()
        || range.offset % 4 != 0)
        return;

    auto* data = static_cast<WebGPUBufferData*>(range.buffer->nativeBuffer());

    if (data == nullptr || data->buffer == nullptr)
        return;

    const auto offset = static_cast<std::uint64_t>(range.offset);
    const auto available = data->size - offset;
    const auto length = std::min(static_cast<std::uint64_t>(range.bytes), available)
                        & ~std::uint64_t {3};

    if (length == 0)
        return;

    if (value == 0)
    {
        impl->recording->encoder.ClearBuffer(data->buffer, offset, length);
        return;
    }

    auto descriptor = wgpu::BufferDescriptor {};
    descriptor.size = length;
    descriptor.usage = wgpu::BufferUsage::CopySrc;
    descriptor.mappedAtCreation = true;

    auto pattern = impl->context.getDevice().CreateBuffer(&descriptor);
    auto* bytes =
        static_cast<std::uint8_t*>(pattern.GetMappedRange(0, toHostSize(length)));

    if (bytes != nullptr)
        std::fill(bytes, bytes + length, value);

    pattern.Unmap();
    impl->recording->encoder.CopyBufferToBuffer(
        pattern, 0, data->buffer, offset, length);
}

void CommandBuffer::submit()
{
    impl->device->assertOwningThread();

    if (impl->canSubmit())
        impl->endAndSubmit();
}

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

    impl->committed = true;
    impl->context.submit(*impl->recording);

    auto done = impl->completed;
    impl->context.getQueue().OnSubmittedWorkDone(
        wgpu::CallbackMode::AllowSpontaneous,
        [done, promise](wgpu::QueueWorkDoneStatus, wgpu::StringView)
        {
            *done = true;
            promise.resolve();
        });

    return promise.get();
}

void CommandBuffer::wait()
{
    impl->device->assertOwningThread();

    if (impl->committed && !*impl->completed)
        impl->context.waitIdle();
}

bool CommandBuffer::isComplete() const
{
    return impl->committed && *impl->completed;
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
    return false;
}

bool CommandBuffer::isValid() const
{
    return impl->recording != nullptr;
}

void* CommandBuffer::nativeRecording() const
{
    return impl->committed ? nullptr : impl->recording.get();
}
} // namespace eacp::GPU

#include "CommandBuffer.h"

#include "../Device/Device.h"
#include "../Linux/GPUBackend-Linux.h"

namespace eacp::GPU
{
struct CommandBuffer::Native
{
    explicit Native(Device& device)
        : backend(getDeviceBackend(device).makeCommandBuffer(device))
    {
    }

    std::unique_ptr<CommandBufferBackend> backend;
};

CommandBuffer::CommandBuffer(Device& device)
    : impl(device)
{
}

ComputePass CommandBuffer::beginCompute(std::string_view label, DispatchOrder order)
{
    return ComputePass(impl->backend->beginCompute(label, order).release(), order);
}

void CommandBuffer::fill(const BufferRange& range, std::uint8_t value)
{
    if (!range.isValid() || range.bytes <= 0 || range.offset < 0
        || range.offset >= range.buffer->size())
        return;

    impl->backend->fill(range, value);
}

void CommandBuffer::submit()
{
    impl->backend->submit();
}

void CommandBuffer::commit()
{
    // Waits, as Metal's commit does; submit() and commitAsync() are how a
    // caller opts out.
    submit();
    wait();
}

Threads::Async<void> CommandBuffer::commitAsync()
{
    return impl->backend->commitAsync();
}

void CommandBuffer::wait()
{
    impl->backend->wait();
}

bool CommandBuffer::isComplete() const
{
    return impl->backend->isComplete();
}

// The wait is scoped to this submission; the copy after it is not, the queue
// being in order, so a readback recorded now still runs behind whatever was
// submitted in between - the same deal the D3D12 backend gets, and for the
// same reason.
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
    return impl->backend->timings();
}

bool CommandBuffer::supportsPassTimings() const
{
    return impl->backend->supportsPassTimings();
}

bool CommandBuffer::isValid() const
{
    return impl->backend->isValid();
}
} // namespace eacp::GPU

#include <eacp/Core/Utils/WinInclude.h>

#include "CommandBuffer.h"

#include "../Device/Device.h"
#include "../Windows/D3D12Types.h"

// Windows/D3D12 backend. Owns one CommandContext recording for its lifetime:
// passes record onto its list, commit() executes it on the direct queue, and
// an uncommitted recording is discarded on destruction. The fence wait inside
// Buffer::read serialises behind the committed work, so a read after commit
// sees the kernel's output.

namespace eacp::GPU
{
struct CommandBuffer::Native
{
    explicit Native(Device& device)
    {
        if (device.isValid())
            open(getD3D12Context().acquire());
    }

    ~Native()
    {
        close();

        if (commands != nullptr && !committed)
            getD3D12Context().discard(commands);
    }

    // Publishes the recording as the one a CPU upload may record onto, for as
    // long as this command buffer is the thing recording - the same courtesy
    // Frame extends, and for the same reason. A buffer filled between here and
    // commit() puts its copy on this list instead of acquiring and submitting
    // one of its own, so a batch that uploads seven buffers before dispatching
    // them is one submission rather than eight.
    void open(CommandContext* commandsToUse)
    {
        commands = commandsToUse;

        if (commands != nullptr)
            getD3D12Context().setOpenRecording(commands);
    }

    // Withdrawn before anything is submitted, so an upload can never be handed
    // a list that has already been closed.
    void close()
    {
        auto& context = getD3D12Context();

        if (commands != nullptr && context.getOpenRecording() == commands)
            context.setOpenRecording(nullptr);
    }

    CommandContext* commands = nullptr;
    bool committed = false;
};

CommandBuffer::CommandBuffer(Device& device)
    : impl(device)
{
}

ComputePass CommandBuffer::beginCompute()
{
    if (impl->commands == nullptr || impl->committed)
        return ComputePass(nullptr);

    // The root signature and heaps are fixed for every compute pipeline, so
    // binding them here frees the pass from caring about setPipeline/set*
    // ordering.
    bindComputeRootState(getD3D12Context(), impl->commands->list.get());

    return ComputePass(new D3D12ComputeEncoder {impl->commands});
}

void CommandBuffer::commit()
{
    if (impl->commands == nullptr || impl->committed)
        return;

    impl->committed = true;
    impl->close();
    getD3D12Context().submit(impl->commands);
}

Threads::Async<void> CommandBuffer::commitAsync()
{
    auto promise = Threads::AsyncPromise<void> {};

    if (impl->commands == nullptr || impl->committed)
    {
        promise.resolve();
        return promise.get();
    }

    impl->committed = true;
    impl->close();

    // submit() already returns without waiting here - what the fence adds is
    // the moment to say so.
    auto& context = getD3D12Context();
    context.notifyWhenCompleted(context.submit(impl->commands),
                                [promise] { promise.resolve(); });

    return promise.get();
}

bool CommandBuffer::isValid() const
{
    return impl->commands != nullptr;
}
} // namespace eacp::GPU

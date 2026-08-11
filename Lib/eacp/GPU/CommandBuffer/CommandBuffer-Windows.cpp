#include <eacp/Core/Utils/WinInclude.h>

#include "CommandBuffer.h"

#include "../Device/Device.h"
#include "../Windows/D3D12Types.h"

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

    // Publishes the recording so CPU uploads join this list instead of
    // acquiring and submitting one each.
    void open(CommandContext* commandsToUse)
    {
        commands = commandsToUse;

        if (commands != nullptr)
            getD3D12Context().setOpenRecording(commands);
    }

    // Withdrawn before submitting, so no upload is handed a closed list.
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

    // Root signature and heaps are fixed for every compute pipeline, so binding
    // here frees the pass from setPipeline/set* ordering.
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

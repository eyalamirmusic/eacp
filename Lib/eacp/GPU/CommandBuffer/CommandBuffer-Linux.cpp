#include "CommandBuffer.h"

#include "../Device/Device.h"
#include "../Vulkan/VulkanTypes.h"

namespace eacp::GPU
{
struct CommandBuffer::Native
{
    explicit Native(Device& device)
        : context(getVulkanContext(device))
    {
        if (context.isValid())
            open(context.acquire());
    }

    ~Native()
    {
        close();

        if (commands != nullptr && !committed)
            context.discard(commands);
    }

    // Publishes the recording as the one a CPU upload may record onto.
    void open(CommandContext* commandsToUse)
    {
        commands = commandsToUse;

        if (commands != nullptr)
            context.setOpenRecording(commands);
    }

    // Withdrawn before the submit, so an upload never gets an ended buffer.
    void close()
    {
        if (commands != nullptr && context.getOpenRecording() == commands)
            context.setOpenRecording(nullptr);
    }

    VulkanContext& context;
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

    return ComputePass(new VulkanComputeEncoder {impl->commands});
}

void CommandBuffer::commit()
{
    if (impl->commands == nullptr || impl->committed)
        return;

    impl->committed = true;
    impl->close();

    // Waits, as Metal's commit does; commitAsync() is how a caller opts out.
    auto& context = impl->context;
    context.waitFor(context.submit(impl->commands));
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

    auto& context = impl->context;
    context.notifyWhenCompleted(context.submit(impl->commands),
                                [promise] { promise.resolve(); });

    return promise.get();
}

bool CommandBuffer::isValid() const
{
    return impl->commands != nullptr;
}
} // namespace eacp::GPU

#include "CommandBuffer.h"

#include "../Device/Device.h"
#include "../Vulkan/VulkanTypes.h"

// Linux/Vulkan backend. Owns one CommandContext recording for its lifetime:
// passes record onto its command buffer, commit() submits it and signals the
// context's timeline, and an uncommitted recording is discarded on destruction.
// The wait inside Buffer::read is on the same timeline, so a read after commit
// sees the kernel's output.

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

    // Publishes the recording as the one a CPU upload may record onto, for as
    // long as this command buffer is the thing recording - the same courtesy
    // Frame extends, and for the same reason. A buffer filled between here and
    // commit() puts its copy on this command buffer instead of acquiring and
    // submitting one of its own, so a batch that uploads seven buffers before
    // dispatching them is one submission rather than eight.
    void open(CommandContext* commandsToUse)
    {
        commands = commandsToUse;

        if (commands != nullptr)
            context.setOpenRecording(commands);
    }

    // Withdrawn before anything is submitted, so an upload can never be handed
    // a command buffer that has already been ended.
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

    // Nothing to bind up front, unlike D3D12: there is no root signature to set
    // and no descriptor heap to name, and the one descriptor set a dispatch
    // needs is built by the pass out of what it was actually given.
    return ComputePass(new VulkanComputeEncoder {impl->commands});
}

void CommandBuffer::commit()
{
    if (impl->commands == nullptr || impl->committed)
        return;

    impl->committed = true;
    impl->close();

    // Waits, because Metal's commit does ([buffer waitUntilCompleted]) and one
    // contract has to hold on every backend. commitAsync() is how a caller opts
    // out of the wait.
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

    // submit() already returns without waiting here - what the timeline adds is
    // the moment to say so.
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

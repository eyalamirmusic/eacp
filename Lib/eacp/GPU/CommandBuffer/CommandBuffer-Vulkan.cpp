#include "CommandBuffer.h"

#include "../Device/Device.h"
#include "../Vulkan/VulkanContext.h"

// The headless sibling of Frame: it owns a recording off the shared context but
// no swapchain, hands it to the compute pass to fill, and blocks on the timeline
// in commit() so a Storage buffer written by the pass is safe to read after.

namespace eacp::GPU
{
struct CommandBuffer::Native
{
    Native()
    {
        auto& context = getVulkanContext();

        if (context.isValid())
            commands = context.acquire();
    }

    CommandContext* commands = nullptr;
};

CommandBuffer::CommandBuffer(Device&)
    : impl()
{
}

ComputePass CommandBuffer::beginCompute()
{
    return ComputePass {impl->commands};
}

void CommandBuffer::commit()
{
    auto& context = getVulkanContext();

    if (impl->commands == nullptr || !context.isValid())
        return;

    context.waitFor(context.submit(impl->commands));
    impl->commands = nullptr;
}

bool CommandBuffer::isValid() const
{
    return impl->commands != nullptr;
}
} // namespace eacp::GPU

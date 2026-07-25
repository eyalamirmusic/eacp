#include "CommandBuffer.h"

#include "../Device/Device.h"
#include "../Vulkan/VulkanContext.h"

// The headless sibling of Frame. Its only consumer today is compute, which this
// backend does not implement yet, so it records and submits an empty recording:
// commit() still orders correctly, and beginCompute yields an inert pass.

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
    return ComputePass {nullptr};
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

#include "ComputePass.h"

#include "../Buffer/Buffer.h"
#include "../Pipeline/ComputePipeline.h"
#include "../Vulkan/VulkanContext.h"
#include "../Vulkan/VulkanTypes.h"

#include <algorithm>

// Vulkan backend. Storage buffers arrive one at a time but a descriptor set is
// written whole, so bindings accumulate and flush at the dispatch that reads
// them -- the same deferral RenderPass uses for its texture set.

namespace eacp::GPU
{
struct ComputePass::Native
{
    explicit Native(void* commandsToUse)
        : commands(static_cast<CommandContext*>(commandsToUse))
    {
    }

    void bind(const Buffer& buffer, int slot)
    {
        if (commands == nullptr || !buffer.isValid() || slot < 0
            || slot >= maxStorageBuffers)
            return;

        auto* native = static_cast<VulkanBuffer*>(buffer.nativeBuffer());

        boundBuffers[slot] = native->buffer;
        boundSizes[slot] = native->bytes;
        buffersDirty = true;
    }

    void flushBuffers()
    {
        if (commands == nullptr || bound == nullptr || !buffersDirty)
            return;

        buffersDirty = false;

        auto& context = getVulkanContext();
        auto set = context.acquireStorageSet(*commands);

        if (set == VK_NULL_HANDLE)
            return;

        auto infos = Vector<VkDescriptorBufferInfo> {};

        for (auto i = 0; i < maxStorageBuffers; ++i)
        {
            if (boundBuffers[i] == VK_NULL_HANDLE)
                continue;

            auto info = VkDescriptorBufferInfo {};
            info.buffer = boundBuffers[i];
            info.range = boundSizes[i];
            infos.add(info);
        }

        auto writes = Vector<VkWriteDescriptorSet> {};
        auto cursor = 0;

        for (auto i = 0; i < maxStorageBuffers; ++i)
        {
            if (boundBuffers[i] == VK_NULL_HANDLE)
                continue;

            auto write = makeVulkanInfo<VkWriteDescriptorSet>(
                VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET);
            write.dstSet = set;
            write.dstBinding = static_cast<std::uint32_t>(i);
            write.descriptorCount = 1;
            write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            write.pBufferInfo = &infos[cursor++];
            writes.add(write);
        }

        if (writes.size() == 0)
            return;

        vkUpdateDescriptorSets(context.getDevice(),
                               static_cast<std::uint32_t>(writes.size()),
                               &writes[0],
                               0,
                               nullptr);

        vkCmdBindDescriptorSets(commands->list,
                                VK_PIPELINE_BIND_POINT_COMPUTE,
                                bound->layout,
                                0,
                                1,
                                &set,
                                0,
                                nullptr);
    }

    CommandContext* commands = nullptr;
    VulkanPipeline* bound = nullptr;
    VkBuffer boundBuffers[maxStorageBuffers] = {};
    VkDeviceSize boundSizes[maxStorageBuffers] = {};
    bool buffersDirty = false;
    bool ended = false;
};

ComputePass::ComputePass(void* commands)
    : impl(commands)
{
}

ComputePass::~ComputePass()
{
    end();
}

void ComputePass::setPipeline(const ComputePipeline& pipeline)
{
    if (impl->commands == nullptr || !pipeline.isValid())
        return;

    impl->bound = static_cast<VulkanPipeline*>(pipeline.nativeState());

    vkCmdBindPipeline(
        impl->commands->list, VK_PIPELINE_BIND_POINT_COMPUTE, impl->bound->pipeline);
}

void ComputePass::setInputBuffer(const Buffer& buffer, int slot)
{
    impl->bind(buffer, slot);
}

void ComputePass::setOutputBuffer(const Buffer& buffer, int slot)
{
    // One descriptor type covers both: the read-only half is marked NonWritable
    // in the shader rather than bound differently, which is where MSL puts the
    // distinction too. Only D3D needs separate view types.
    impl->bind(buffer, slot);
}

void ComputePass::setBytes(const void* data, std::size_t bytes, int slot)
{
    if (impl->commands == nullptr || impl->bound == nullptr || data == nullptr
        || slot != 0)
        return;

    auto capped =
        std::min(bytes, static_cast<std::size_t>(impl->bound->pushConstantBytes));

    vkCmdPushConstants(impl->commands->list,
                       impl->bound->layout,
                       VK_SHADER_STAGE_COMPUTE_BIT,
                       0,
                       static_cast<std::uint32_t>(capped),
                       data);
}

void ComputePass::dispatch(int count)
{
    if (impl->commands == nullptr || impl->bound == nullptr || count <= 0)
        return;

    impl->flushBuffers();

    auto groups = (count + threadGroupWidth - 1) / threadGroupWidth;

    vkCmdDispatch(impl->commands->list, static_cast<std::uint32_t>(groups), 1, 1);
}

void ComputePass::end()
{
    if (impl->commands == nullptr || impl->ended)
        return;

    impl->ended = true;

    // Storage buffers are host-visible and read back through their mapping the
    // moment commit() returns, which needs the writes made visible to the host
    // domain -- coherent memory alone does not order them.
    auto barrier = makeVulkanInfo<VkMemoryBarrier>(VK_STRUCTURE_TYPE_MEMORY_BARRIER);
    barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_HOST_READ_BIT;

    vkCmdPipelineBarrier(impl->commands->list,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         VK_PIPELINE_STAGE_HOST_BIT,
                         0,
                         1,
                         &barrier,
                         0,
                         nullptr,
                         0,
                         nullptr);
}
} // namespace eacp::GPU

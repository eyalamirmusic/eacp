#include "ComputePass.h"

#include "../Buffer/Buffer.h"
#include "../Pipeline/ComputePipeline.h"
#include "../Vulkan/VulkanBackend-Linux.h"
#include "../Vulkan/VulkanTypes.h"

#include <memory>

namespace eacp::GPU
{
namespace
{
struct VulkanComputePassBackend final : ComputePassBackend
{
    VulkanComputePassBackend(VulkanComputeEncoder* encoderToUse,
                             DispatchOrder dispatchOrder)
        : encoder(encoderToUse)
        , order(dispatchOrder)
    {
    }

    ~VulkanComputePassBackend() override { end(); }

    bool canRecord() const { return encoder != nullptr && pipeline != nullptr; }

    bool isConcurrent() const { return order == DispatchOrder::Concurrent; }

    VkCommandBuffer commandBuffer() const { return encoder->commands->buffer; }

    void orderAfterDispatch(VkCommandBuffer buffer) const
    {
        if (!isConcurrent())
            barrierAfterDispatch(buffer);
    }

    void recordBarrier() const
    {
        if (encoder != nullptr && encoder->commands != nullptr)
            barrierAfterDispatch(encoder->commands->buffer);
    }

    // Writes have to land before the set is bound, so binds are collected and
    // written here, at the dispatch, where the pipeline's layout is known.
    bool bindDescriptors()
    {
        auto& commands = *encoder->commands;

        auto set =
            commands.context->allocateDescriptorSet(commands, pipeline->setLayout);

        if (set == VK_NULL_HANDLE)
            return false;

        VkWriteDescriptorSet writes[maxBufferSlots + maxTextureSlots + 1] = {};
        auto writeCount = std::uint32_t {0};

        for (auto slot = 0; slot < maxBufferSlots; ++slot)
        {
            if ((boundBuffers & (1u << slot)) == 0)
                continue;

            auto& write = writes[writeCount++];
            write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            write.dstSet = set;
            write.dstBinding =
                static_cast<std::uint32_t>(vulkanComputeBufferBinding(slot));
            write.descriptorCount = 1;
            write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            write.pBufferInfo = &buffers[slot];
        }

        // The type the kernel declared is the only type a write to that binding
        // may name, so a bind made the other way is dropped.
        for (auto slot = 0; slot < maxTextureSlots; ++slot)
        {
            if (!pipeline->textures.has(slot))
                continue;

            const auto type = pipeline->textures.typeAt(slot);
            const auto bound =
                type == VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER ? sampledTextures
                : type == VK_DESCRIPTOR_TYPE_STORAGE_IMAGE        ? storageTextures
                                                                  : 0u;

            if ((bound & (1u << slot)) == 0)
                continue;

            auto& write = writes[writeCount++];
            write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            write.dstSet = set;
            write.dstBinding =
                static_cast<std::uint32_t>(vulkanComputeTextureBinding(slot));
            write.descriptorCount = 1;
            write.descriptorType = type;
            write.pImageInfo = &textures[slot];
        }

        VkDescriptorBufferInfo uniformInfo = {};

        if (uniforms.isValid())
        {
            // Offset zero; the dynamic offset below carries the block's place.
            uniformInfo.buffer = uniforms.buffer;
            uniformInfo.offset = 0;
            uniformInfo.range = uniforms.range;

            auto& write = writes[writeCount++];
            write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            write.dstSet = set;
            write.dstBinding =
                static_cast<std::uint32_t>(vulkanComputeUniformBinding);
            write.descriptorCount = 1;
            write.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
            write.pBufferInfo = &uniformInfo;
        }

        if (writeCount > 0)
            vkUpdateDescriptorSets(
                commands.context->getDevice(), writeCount, writes, 0, nullptr);

        // The layout always declares one dynamic descriptor, so one offset has
        // to be passed even with no uniform block bound.
        const auto dynamicOffset = static_cast<std::uint32_t>(uniforms.offset);

        vkCmdBindDescriptorSets(commands.buffer,
                                VK_PIPELINE_BIND_POINT_COMPUTE,
                                pipeline->layout,
                                0,
                                1,
                                &set,
                                1,
                                &dynamicOffset);
        return true;
    }

    std::unique_ptr<VulkanComputeEncoder> encoder;

    DispatchOrder order = DispatchOrder::Serial;

    const VulkanComputePipeline* pipeline = nullptr;

    VkDescriptorBufferInfo buffers[maxBufferSlots] = {};
    std::uint32_t boundBuffers = 0;

    VkDescriptorImageInfo textures[maxTextureSlots] = {};
    std::uint32_t sampledTextures = 0;
    std::uint32_t storageTextures = 0;

    ConstantRange uniforms;
    bool setPipeline(const ComputePipeline& pipeline) override
    {
        if (!encoder)
            return false;

        auto* state = static_cast<VulkanComputePipeline*>(pipeline.nativeState());

        if (state == nullptr || state->pipeline == VK_NULL_HANDLE)
            return false;

        this->pipeline = state;

        vkCmdBindPipeline(
            commandBuffer(), VK_PIPELINE_BIND_POINT_COMPUTE, state->pipeline);

        return true;
    }

    void setInputBuffer(const BufferRange& range, int slot) override
    {
        bindBuffer(range, slot, bufferShaderRead);
    }

    void setOutputBuffer(const BufferRange& range, int slot) override
    {
        bindBuffer(range, slot, bufferShaderWrite);
    }

    void bindBuffer(const BufferRange& range, int slot, const BufferUse& use)
    {
        if (!encoder || slot < 0 || slot >= maxBufferSlots
            || range.buffer == nullptr)
            return;

        auto* data = static_cast<VulkanBufferData*>(range.buffer->nativeBuffer());
        const auto info = vulkanStorageBufferInfo(data, range);

        if (info.buffer == VK_NULL_HANDLE)
            return;

        transitionForUse(*encoder->commands, *data, use);

        buffers[slot] = info;
        boundBuffers |= 1u << slot;
    }

    void setInputTexture(const Texture& texture,
                         int slot,
                         TextureSampling sampling) override
    {
        if (!encoder || slot < 0 || slot >= maxTextureSlots)
            return;

        auto* data = static_cast<VulkanTextureData*>(texture.nativeTexture());

        if (data == nullptr || !data->isValid())
            return;

        const auto sampler = getVulkanShared().getSampler(sampling);

        if (sampler == VK_NULL_HANDLE)
            return;

        const auto& target = data->restingUse();
        transitionTextureForUse(commandBuffer(), *data, target);

        textures[slot] = {sampler, data->sampledView, target.layout};
        sampledTextures |= 1u << slot;
        storageTextures &= ~(1u << slot);
    }

    void setOutputTexture(const Texture& texture, int slot) override
    {
        if (!encoder || slot < 0 || slot >= maxTextureSlots)
            return;

        auto* data = static_cast<VulkanTextureData*>(texture.nativeTexture());

        if (data == nullptr || !data->isValid() || !data->isComputeWritable())
            return;

        transitionTextureForUse(commandBuffer(), *data, imageStorage);

        textures[slot] = {VK_NULL_HANDLE, data->storageView, imageStorage.layout};
        storageTextures |= 1u << slot;
        sampledTextures &= ~(1u << slot);
    }

    void setBytes(const void* data, std::int64_t bytes, int slot) override
    {
        if (!encoder || bytes <= 0 || slot < 0 || slot >= maxUniformSlots)
            return;

        auto& commands = *encoder->commands;
        uniforms =
            commands.context->uploadConstants(commands, data, (std::size_t) bytes);
    }

    void dispatch(int width, int height, int depth, ThreadGroupShape group) override
    {
        if (!canRecord() || !bindDescriptors())
            return;

        const auto groupsFor = [](int extent, int size)
        {
            const auto span = static_cast<std::uint32_t>(size > 0 ? size : 1);

            return (static_cast<std::uint32_t>(extent) + span - 1) / span;
        };

        auto commands = commandBuffer();

        vkCmdDispatch(commands,
                      groupsFor(width, group.x),
                      groupsFor(height, group.y),
                      groupsFor(depth, group.z));

        orderAfterDispatch(commands);
    }

    void dispatchIndirect(const Buffer& arguments,
                          std::int64_t offsetInBytes) override
    {
        if (!canRecord())
            return;

        auto* data = static_cast<VulkanBufferData*>(arguments.nativeBuffer());

        if (data == nullptr || data->buffer == VK_NULL_HANDLE)
            return;

        // The arguments come out of a kernel that may still be running, and the
        // transition below says nothing about a buffer already in the state it
        // wants.
        if (isConcurrent())
            recordBarrier();

        transitionForUse(*encoder->commands, *data, bufferIndirectRead);

        if (!bindDescriptors())
            return;

        auto commands = commandBuffer();
        vkCmdDispatchIndirect(
            commands, data->buffer, static_cast<VkDeviceSize>(offsetInBytes));
        orderAfterDispatch(commands);
    }

    void barrier() override
    {
        if (isConcurrent())
            recordBarrier();
    }

    // A concurrent pass owes the rest of the recording what the per-dispatch
    // barriers owed it in a serial one, so the last dispatches are ordered here
    // against whatever the next pass or a readback copy does.
    void end() override
    {
        if (encoder)
        {
            if (isConcurrent())
                recordBarrier();

            endTimedPass(*encoder);
        }

        encoder.reset();
        pipeline = nullptr;
    }
};
} // namespace

std::unique_ptr<ComputePassBackend> makeVulkanComputePass(
    VulkanComputeEncoder* encoder, DispatchOrder order)
{
    return std::make_unique<VulkanComputePassBackend>(encoder, order);
}
} // namespace eacp::GPU

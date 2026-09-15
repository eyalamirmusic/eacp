#include "ComputePass.h"

#include "../Buffer/Buffer.h"
#include "../Pipeline/ComputePipeline.h"
#include "../Vulkan/VulkanTypes.h"

#include <memory>

namespace eacp::GPU
{
struct ComputePass::Native
{
    Native(void* encoderHandle, DispatchOrder dispatchOrder)
        : encoder(static_cast<VulkanComputeEncoder*>(encoderHandle))
        , order(dispatchOrder)
    {
    }

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
};

ComputePass::ComputePass(void* encoder, DispatchOrder order)
    : impl(encoder, order)
{
}

ComputePass::~ComputePass()
{
    end();
}

void ComputePass::setPipeline(const ComputePipeline& pipeline)
{
    boundGroup = pipeline.threadGroupShape();

    if (!impl->encoder)
        return;

    auto* state = static_cast<VulkanComputePipeline*>(pipeline.nativeState());

    if (state == nullptr || state->pipeline == VK_NULL_HANDLE)
        return;

    impl->pipeline = state;
    vkCmdBindPipeline(
        impl->commandBuffer(), VK_PIPELINE_BIND_POINT_COMPUTE, state->pipeline);
}

void ComputePass::setInputBuffer(const Buffer& buffer, int slot)
{
    setInputBuffer(BufferRange::of(buffer), slot);
}

void ComputePass::setInputBuffer(const BufferRange& range, int slot)
{
    if (!impl->encoder || slot < 0 || slot >= maxBufferSlots
        || range.buffer == nullptr)
        return;

    auto* data = static_cast<VulkanBufferData*>(range.buffer->nativeBuffer());
    const auto info = vulkanStorageBufferInfo(data, range);

    if (info.buffer == VK_NULL_HANDLE)
        return;

    transitionForUse(*impl->encoder->commands, *data, bufferShaderRead);

    impl->buffers[slot] = info;
    impl->boundBuffers |= 1u << slot;
}

void ComputePass::setOutputBuffer(const Buffer& buffer, int slot)
{
    setOutputBuffer(BufferRange::of(buffer), slot);
}

void ComputePass::setOutputBuffer(const BufferRange& range, int slot)
{
    if (!impl->encoder || slot < 0 || slot >= maxBufferSlots
        || range.buffer == nullptr)
        return;

    auto* data = static_cast<VulkanBufferData*>(range.buffer->nativeBuffer());
    const auto info = vulkanStorageBufferInfo(data, range);

    if (info.buffer == VK_NULL_HANDLE)
        return;

    transitionForUse(*impl->encoder->commands, *data, bufferShaderWrite);

    impl->buffers[slot] = info;
    impl->boundBuffers |= 1u << slot;
}

void ComputePass::setInputTexture(const Texture& texture,
                                  int slot,
                                  TextureSampling sampling)
{
    if (!impl->encoder || slot < 0 || slot >= maxTextureSlots)
        return;

    auto* data = static_cast<VulkanTextureData*>(texture.nativeTexture());

    if (data == nullptr || !data->isValid())
        return;

    const auto sampler = getVulkanShared().getSampler(sampling);

    if (sampler == VK_NULL_HANDLE)
        return;

    const auto& target = data->restingUse();
    transitionTextureForUse(impl->commandBuffer(), *data, target);

    impl->textures[slot] = {sampler, data->sampledView, target.layout};
    impl->sampledTextures |= 1u << slot;
    impl->storageTextures &= ~(1u << slot);
}

void ComputePass::setOutputTexture(const Texture& texture, int slot)
{
    if (!impl->encoder || slot < 0 || slot >= maxTextureSlots)
        return;

    auto* data = static_cast<VulkanTextureData*>(texture.nativeTexture());

    if (data == nullptr || !data->isValid() || !data->isComputeWritable())
        return;

    transitionTextureForUse(impl->commandBuffer(), *data, imageStorage);

    impl->textures[slot] = {VK_NULL_HANDLE, data->storageView, imageStorage.layout};
    impl->storageTextures |= 1u << slot;
    impl->sampledTextures &= ~(1u << slot);
}

void ComputePass::setBytes(const void* data, int bytes, int slot)
{
    if (!impl->encoder || bytes <= 0 || slot < 0 || slot >= maxUniformSlots)
        return;

    auto& commands = *impl->encoder->commands;
    impl->uniforms =
        commands.context->uploadConstants(commands, data, (std::size_t) bytes);
}

void ComputePass::dispatch(int count)
{
    if (!impl->canRecord() || count <= 0)
        return;

    if (!impl->bindDescriptors())
        return;

    const auto width = static_cast<std::uint32_t>(groupFor1D().x);
    const auto groups = (static_cast<std::uint32_t>(count) + width - 1) / width;

    auto commandBuffer = impl->commandBuffer();
    vkCmdDispatch(commandBuffer, groups, 1, 1);
    impl->orderAfterDispatch(commandBuffer);
}

void ComputePass::dispatch(int width, int height)
{
    if (!impl->canRecord() || width <= 0 || height <= 0)
        return;

    if (!impl->bindDescriptors())
        return;

    const auto group = groupFor2D();
    const auto sizeX = static_cast<std::uint32_t>(group.x);
    const auto sizeY = static_cast<std::uint32_t>(group.y);
    const auto groupsX = (static_cast<std::uint32_t>(width) + sizeX - 1) / sizeX;
    const auto groupsY = (static_cast<std::uint32_t>(height) + sizeY - 1) / sizeY;

    auto commandBuffer = impl->commandBuffer();
    vkCmdDispatch(commandBuffer, groupsX, groupsY, 1);
    impl->orderAfterDispatch(commandBuffer);
}

void ComputePass::dispatch(int width, int height, int depth)
{
    if (!impl->canRecord() || width <= 0 || height <= 0 || depth <= 0)
        return;

    if (!impl->bindDescriptors())
        return;

    const auto group = groupFor3D();
    const auto sizeX = static_cast<std::uint32_t>(group.x);
    const auto sizeY = static_cast<std::uint32_t>(group.y);
    const auto sizeZ = static_cast<std::uint32_t>(group.z);
    const auto groupsX = (static_cast<std::uint32_t>(width) + sizeX - 1) / sizeX;
    const auto groupsY = (static_cast<std::uint32_t>(height) + sizeY - 1) / sizeY;
    const auto groupsZ = (static_cast<std::uint32_t>(depth) + sizeZ - 1) / sizeZ;

    auto commandBuffer = impl->commandBuffer();
    vkCmdDispatch(commandBuffer, groupsX, groupsY, groupsZ);
    impl->orderAfterDispatch(commandBuffer);
}

void ComputePass::dispatchIndirect(const Buffer& arguments, int offsetInBytes)
{
    if (!impl->canRecord() || offsetInBytes < 0 || offsetInBytes % 4 != 0
        || offsetInBytes > arguments.size() - (int) sizeof(DispatchArguments))
        return;

    auto* data = static_cast<VulkanBufferData*>(arguments.nativeBuffer());

    if (data == nullptr || data->buffer == VK_NULL_HANDLE)
        return;

    // The arguments come out of a kernel that may still be running, and the
    // transition below says nothing about a buffer already in the state it
    // wants.
    if (impl->isConcurrent())
        impl->recordBarrier();

    transitionForUse(*impl->encoder->commands, *data, bufferIndirectRead);

    if (!impl->bindDescriptors())
        return;

    auto commandBuffer = impl->commandBuffer();
    vkCmdDispatchIndirect(
        commandBuffer, data->buffer, static_cast<VkDeviceSize>(offsetInBytes));
    impl->orderAfterDispatch(commandBuffer);
}

void ComputePass::barrier()
{
    if (impl->isConcurrent())
        impl->recordBarrier();
}

// A concurrent pass owes the rest of the recording what the per-dispatch
// barriers owed it in a serial one, so the last dispatches are ordered here
// against whatever the next pass or a readback copy does.
void ComputePass::end()
{
    if (impl->encoder)
    {
        if (impl->isConcurrent())
            impl->recordBarrier();

        endTimedPass(*impl->encoder);
    }

    impl->encoder.reset();
    impl->pipeline = nullptr;
}
} // namespace eacp::GPU

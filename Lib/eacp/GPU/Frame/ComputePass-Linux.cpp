#include "ComputePass.h"

#include "../Buffer/Buffer.h"
#include "../Pipeline/ComputePipeline.h"
#include "../Vulkan/VulkanTypes.h"

#include <memory>

// Linux/Vulkan backend. Records onto the command buffer's recording via the
// VulkanComputeEncoder.
//
// Where D3D12 binds a buffer as a root descriptor by GPU address and needs no
// heap at all, Vulkan has one binding model for everything: a descriptor set,
// allocated out of the recording's pool, written with whatever the pass was
// given and bound before the dispatch. So the binds are collected rather than
// recorded - a bind is a descriptor write, and a write after the set is bound
// is a write the dispatch may or may not see - and the set is built at the
// dispatch, when the pipeline (and therefore the layout it must match) is
// finally known.
//
// A set per dispatch rather than one reused: the pool is reset when the
// recording is, so a set costs a pool allocation and nothing else, and a
// recording that outgrows a pool gets another. The uniform block is the one
// binding that does not need a new descriptor - it is a UNIFORM_BUFFER_DYNAMIC,
// so a recording's dispatches share one constant page and differ in the offset
// handed to vkCmdBindDescriptorSets.
//
// A memory barrier after every dispatch orders chained kernels, exactly as the
// D3D12 backend's UAV barrier does.

namespace eacp::GPU
{
struct ComputePass::Native
{
    explicit Native(void* encoderHandle)
        : encoder(static_cast<VulkanComputeEncoder*>(encoderHandle))
    {
    }

    bool canRecord() const { return encoder != nullptr && pipeline != nullptr; }

    VkCommandBuffer commandBuffer() const { return encoder->commands->buffer; }

    // Everything bound since the last dispatch, written into a fresh set and
    // bound. False when the set could not be had, which stops the dispatch
    // rather than running it against whatever was bound before.
    bool bindDescriptors()
    {
        auto& commands = *encoder->commands;

        auto set =
            commands.context->allocateDescriptorSet(commands, pipeline->setLayout);

        if (set == VK_NULL_HANDLE)
            return false;

        VkWriteDescriptorSet writes[maxBufferSlots + 1] = {};
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

        VkDescriptorBufferInfo uniformInfo = {};

        if (uniforms.isValid())
        {
            // Offset zero and the dynamic offset carrying the whole of it: the
            // page is one buffer for the recording, and where in it this block
            // sits is exactly what a dynamic offset is for.
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

        // One dynamic offset always, whether or not a uniform block was bound:
        // the layout declares one dynamic descriptor, and the count has to
        // match it. Zero is a legal offset into a page nothing reads.
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

    const VulkanComputePipeline* pipeline = nullptr;

    VkDescriptorBufferInfo buffers[maxBufferSlots] = {};
    std::uint32_t boundBuffers = 0;

    ConstantRange uniforms;
};

ComputePass::ComputePass(void* encoder)
    : impl(encoder)
{
}

ComputePass::~ComputePass()
{
    end();
}

void ComputePass::setPipeline(const ComputePipeline& pipeline)
{
    if (!impl->encoder)
        return;

    auto* state = static_cast<VulkanComputePipeline*>(pipeline.nativeState());

    if (state == nullptr || state->pipeline == VK_NULL_HANDLE)
        return;

    impl->pipeline = state;
    vkCmdBindPipeline(
        impl->commandBuffer(), VK_PIPELINE_BIND_POINT_COMPUTE, state->pipeline);
}

// Both directions are one STORAGE_BUFFER descriptor over the whole buffer - a
// GLSL std430 block is the same declaration either way, so unlike D3D12 there
// is no read view and no write view to pick between. What differs is the
// barrier, which is the whole of why these are two functions.
void ComputePass::setInputBuffer(const Buffer& buffer, int slot)
{
    if (!impl->encoder || slot < 0 || slot >= maxBufferSlots)
        return;

    auto* data = static_cast<VulkanBufferData*>(buffer.nativeBuffer());

    if (data == nullptr || data->buffer == VK_NULL_HANDLE)
        return;

    transitionForUse(*impl->encoder->commands, *data, bufferShaderRead);

    impl->buffers[slot] = {data->buffer, 0, VK_WHOLE_SIZE};
    impl->boundBuffers |= 1u << slot;
}

void ComputePass::setOutputBuffer(const Buffer& buffer, int slot)
{
    if (!impl->encoder || slot < 0 || slot >= maxBufferSlots)
        return;

    auto* data = static_cast<VulkanBufferData*>(buffer.nativeBuffer());

    if (data == nullptr || data->buffer == VK_NULL_HANDLE)
        return;

    transitionForUse(*impl->encoder->commands, *data, bufferShaderWrite);

    impl->buffers[slot] = {data->buffer, 0, VK_WHOLE_SIZE};
    impl->boundBuffers |= 1u << slot;
}

// Both texture binds are no-ops until stage 3. A Texture on Linux is a
// placeholder with nothing behind it, so there is no image and no view to write
// into a descriptor - and a kernel that declares one is already refused a
// pipeline (ComputePipeline-Linux.cpp), so nothing reaches a dispatch expecting
// a texture it did not get.
void ComputePass::setInputTexture(const Texture&, int, TextureSampling) {}

void ComputePass::setOutputTexture(const Texture&, int) {}

void ComputePass::setBytes(const void* data, std::size_t bytes, int slot)
{
    if (!impl->encoder || slot < 0 || slot >= maxUniformSlots)
        return;

    auto& commands = *impl->encoder->commands;
    impl->uniforms = commands.context->uploadConstants(commands, data, bytes);
}

void ComputePass::dispatch(int count)
{
    if (!impl->canRecord() || count <= 0)
        return;

    if (!impl->bindDescriptors())
        return;

    const auto groups = (static_cast<std::uint32_t>(count) + threadGroupWidth - 1)
                        / threadGroupWidth;

    auto commandBuffer = impl->commandBuffer();
    vkCmdDispatch(commandBuffer, groups, 1, 1);
    barrierAfterDispatch(commandBuffer);
}

void ComputePass::dispatch(int width, int height)
{
    if (!impl->canRecord() || width <= 0 || height <= 0)
        return;

    if (!impl->bindDescriptors())
        return;

    const auto size = static_cast<std::uint32_t>(threadGroupSize2D);
    const auto groupsX = (static_cast<std::uint32_t>(width) + size - 1) / size;
    const auto groupsY = (static_cast<std::uint32_t>(height) + size - 1) / size;

    auto commandBuffer = impl->commandBuffer();
    vkCmdDispatch(commandBuffer, groupsX, groupsY, 1);
    barrierAfterDispatch(commandBuffer);
}

// The grid comes out of the buffer; the threadgroup size is baked into the
// shader's local_size and is not part of the arguments, which is why
// VkDispatchIndirectCommand holds only the three counts - the same three
// DispatchArguments holds, at the same size and in the same order.
//
// The buffer needs a barrier of its own here. An earlier kernel wrote it as a
// storage buffer, and reading it as indirect arguments is a different access at
// a different stage - the one transition Metal has no equivalent of.
void ComputePass::dispatchIndirect(const Buffer& arguments, int offsetInBytes)
{
    if (!impl->canRecord() || offsetInBytes < 0)
        return;

    auto* data = static_cast<VulkanBufferData*>(arguments.nativeBuffer());

    if (data == nullptr || data->buffer == VK_NULL_HANDLE)
        return;

    transitionForUse(*impl->encoder->commands, *data, bufferIndirectRead);

    if (!impl->bindDescriptors())
        return;

    auto commandBuffer = impl->commandBuffer();
    vkCmdDispatchIndirect(
        commandBuffer, data->buffer, static_cast<VkDeviceSize>(offsetInBytes));
    barrierAfterDispatch(commandBuffer);
}

void ComputePass::end()
{
    if (impl->encoder)
        endTimedPass(*impl->encoder);

    impl->encoder.reset();
    impl->pipeline = nullptr;
}
} // namespace eacp::GPU

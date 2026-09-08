#include "RenderPass.h"

#include "../Buffer/Buffer.h"
#include "../Pipeline/RenderPipeline.h"
#include "../Texture/Texture.h"
#include "../Vulkan/VulkanTypes.h"

#include <algorithm>
#include <cmath>
#include <memory>

namespace eacp::GPU
{
struct RenderPass::Native
{
    Native(void* encoderHandle, int width, int height)
        : encoder(static_cast<VulkanRenderEncoder*>(encoderHandle))
        , targetWidth(width)
        , targetHeight(height)
    {
    }

    VkCommandBuffer commandBuffer() const { return encoder->commands->buffer; }

    bool canRecord() const
    {
        return encoder != nullptr && encoder->pipeline != nullptr
               && encoder->pipeline->pipeline != VK_NULL_HANDLE;
    }

    // Writes have to land before the set is bound, so binds are collected and
    // written here, at the draw.
    bool bindDescriptors()
    {
        if (!descriptorsDirty)
            return true;

        auto& commands = *encoder->commands;
        const auto& pipeline = *encoder->pipeline;

        auto set =
            commands.context->allocateDescriptorSet(commands, pipeline.setLayout);

        if (set == VK_NULL_HANDLE)
            return false;

        VkWriteDescriptorSet writes[maxBufferSlots + maxTextureSlots + 1] = {};
        auto writeCount = std::uint32_t {0};

        for (auto slot = 0; slot < maxTextureSlots; ++slot)
        {
            if ((boundTextures & (1u << slot)) == 0)
                continue;

            auto& write = writes[writeCount++];
            write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            write.dstSet = set;
            write.dstBinding =
                static_cast<std::uint32_t>(vulkanTextureBinding(slot));
            write.descriptorCount = 1;
            write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            write.pImageInfo = &textures[slot];
        }

        for (auto slot = 0; slot < maxBufferSlots; ++slot)
        {
            if ((boundBuffers & (1u << slot)) == 0)
                continue;

            auto& write = writes[writeCount++];
            write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            write.dstSet = set;
            write.dstBinding = static_cast<std::uint32_t>(vulkanBufferBinding(slot));
            write.descriptorCount = 1;
            write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            write.pBufferInfo = &buffers[slot];
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
            write.dstBinding = static_cast<std::uint32_t>(vulkanUniformBinding);
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
                                VK_PIPELINE_BIND_POINT_GRAPHICS,
                                pipeline.layout,
                                0,
                                1,
                                &set,
                                1,
                                &dynamicOffset);

        descriptorsDirty = false;
        return true;
    }

    bool bindIndexRange(const BufferRange& indices, IndexFormat format)
    {
        if (indices.buffer == nullptr)
            return false;

        auto* data = static_cast<VulkanBufferData*>(indices.buffer->nativeBuffer());

        if (data == nullptr || data->buffer == VK_NULL_HANDLE || indices.offset < 0
            || static_cast<std::size_t>(indices.offset) >= data->size)
            return false;

        noteBufferUse(*encoder->commands, *data, bufferIndexRead);

        vkCmdBindIndexBuffer(commandBuffer(),
                             data->buffer,
                             static_cast<VkDeviceSize>(indices.offset),
                             format == IndexFormat::UInt16 ? VK_INDEX_TYPE_UINT16
                                                           : VK_INDEX_TYPE_UINT32);
        return true;
    }

    void bindStorageBuffer(const BufferRange& range, int slot)
    {
        if (encoder == nullptr || slot < 0 || slot >= maxBufferSlots
            || range.buffer == nullptr)
            return;

        auto* data = static_cast<VulkanBufferData*>(range.buffer->nativeBuffer());
        const auto info = vulkanStorageBufferInfo(data, range);

        if (info.buffer == VK_NULL_HANDLE)
            return;

        noteBufferUse(*encoder->commands, *data, bufferGraphicsRead);

        buffers[slot] = info;
        boundBuffers |= 1u << slot;
        descriptorsDirty = true;
    }

    void uploadUniforms(const void* data, int bytes, int slot)
    {
        if (encoder == nullptr || bytes <= 0 || slot < 0 || slot >= maxUniformSlots)
            return;

        auto& commands = *encoder->commands;
        uniforms =
            commands.context->uploadConstants(commands, data, (std::size_t) bytes);
        descriptorsDirty = true;
    }

    std::unique_ptr<VulkanRenderEncoder> encoder;

    int targetWidth = 0;
    int targetHeight = 0;

    VkDescriptorBufferInfo buffers[maxBufferSlots] = {};
    std::uint32_t boundBuffers = 0;

    VkDescriptorImageInfo textures[maxTextureSlots] = {};
    std::uint32_t boundTextures = 0;

    ConstantRange uniforms;

    bool descriptorsDirty = true;
};

RenderPass::RenderPass(void* encoder, int targetWidth, int targetHeight)
    : impl(encoder, targetWidth, targetHeight)
{
}

RenderPass::~RenderPass()
{
    end();
}

void RenderPass::setScissorRect(const Graphics::Rect& rect)
{
    if (!impl->encoder || impl->targetWidth <= 0 || impl->targetHeight <= 0)
        return;

    // Outward: rounding an edge inward would shave a column of coverage off it.
    const auto left =
        std::clamp(static_cast<int>(std::floor(rect.x)), 0, impl->targetWidth);
    const auto top =
        std::clamp(static_cast<int>(std::floor(rect.y)), 0, impl->targetHeight);
    const auto right = std::clamp(
        static_cast<int>(std::ceil(rect.x + rect.w)), left, impl->targetWidth);
    const auto bottom = std::clamp(
        static_cast<int>(std::ceil(rect.y + rect.h)), top, impl->targetHeight);

    // Framebuffer coordinates, y down: the negative viewport height flips clip
    // space and nothing else.
    const VkRect2D scissor {{left, top},
                            {static_cast<std::uint32_t>(right - left),
                             static_cast<std::uint32_t>(bottom - top)}};

    vkCmdSetScissor(impl->commandBuffer(), 0, 1, &scissor);
}

void RenderPass::clearScissorRect()
{
    if (!impl->encoder || impl->targetWidth <= 0 || impl->targetHeight <= 0)
        return;

    const VkRect2D scissor {{0, 0},
                            {static_cast<std::uint32_t>(impl->targetWidth),
                             static_cast<std::uint32_t>(impl->targetHeight)}};

    vkCmdSetScissor(impl->commandBuffer(), 0, 1, &scissor);
}

// The negative height answers Vulkan's y-down NDC; winding and every shader in
// the tree depend on it.
void RenderPass::setViewport(const Graphics::Rect& rect,
                             float nearDepth,
                             float farDepth)
{
    if (!impl->encoder || impl->targetWidth <= 0 || impl->targetHeight <= 0)
        return;

    if (rect.w <= 0.f || rect.h <= 0.f || rect.x < 0.f || rect.y < 0.f
        || rect.x + rect.w > static_cast<float>(impl->targetWidth)
        || rect.y + rect.h > static_cast<float>(impl->targetHeight))
        return;

    const VkViewport viewport {
        rect.x, rect.y + rect.h, rect.w, -rect.h, nearDepth, farDepth};

    vkCmdSetViewport(impl->commandBuffer(), 0, 1, &viewport);
}

void RenderPass::clearViewport()
{
    if (!impl->encoder || impl->targetWidth <= 0 || impl->targetHeight <= 0)
        return;

    const VkViewport viewport {0.f,
                               static_cast<float>(impl->targetHeight),
                               static_cast<float>(impl->targetWidth),
                               -static_cast<float>(impl->targetHeight),
                               0.f,
                               1.f};

    vkCmdSetViewport(impl->commandBuffer(), 0, 1, &viewport);
}

int RenderPass::targetWidth() const
{
    return impl->targetWidth;
}

int RenderPass::targetHeight() const
{
    return impl->targetHeight;
}

void RenderPass::setPipeline(const RenderPipeline& pipeline)
{
    if (!impl->encoder)
        return;

    auto* state = static_cast<VulkanRenderPipeline*>(pipeline.nativeState());

    impl->encoder->pipeline = nullptr;

    if (state == nullptr || state->pipeline == VK_NULL_HANDLE)
        return;

    impl->encoder->pipeline = state;

    vkCmdBindPipeline(
        impl->commandBuffer(), VK_PIPELINE_BIND_POINT_GRAPHICS, state->pipeline);

    // The set that was bound belongs to the previous pipeline's layout.
    impl->descriptorsDirty = true;
}

void RenderPass::setStencilReference(unsigned int value)
{
    if (!impl->encoder)
        return;

    vkCmdSetStencilReference(impl->commandBuffer(),
                             VK_STENCIL_FACE_FRONT_AND_BACK,
                             static_cast<std::uint32_t>(value));
}

void RenderPass::setVertexBuffer(const Buffer& buffer, int index)
{
    setVertexBuffer(BufferRange::of(buffer), index);
}

void RenderPass::setVertexBuffer(const BufferRange& range, int index)
{
    if (!impl->encoder || range.buffer == nullptr || index < 0)
        return;

    auto* data = static_cast<VulkanBufferData*>(range.buffer->nativeBuffer());

    if (data == nullptr || data->buffer == VK_NULL_HANDLE || range.offset < 0
        || static_cast<std::size_t>(range.offset) >= data->size)
        return;

    noteBufferUse(*impl->encoder->commands, *data, bufferVertexRead);

    const auto offset = static_cast<VkDeviceSize>(range.offset);

    vkCmdBindVertexBuffers(impl->commandBuffer(),
                           static_cast<std::uint32_t>(index),
                           1,
                           &data->buffer,
                           &offset);
}

// No barrier: Vulkan forbids one inside a render pass instance, and the
// resting-layout rule on VulkanTextureData leaves the image ready to sample.
void RenderPass::setFragmentTexture(const Texture& texture,
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

    impl->textures[slot] = {sampler, data->sampledView, data->restingUse().layout};
    impl->boundTextures |= 1u << slot;
    impl->descriptorsDirty = true;
}

void RenderPass::setFragmentDepthTexture(const Texture& renderTarget,
                                         int slot,
                                         TextureSampling sampling)
{
    if (!impl->encoder || slot < 0 || slot >= maxTextureSlots)
        return;

    auto* data = static_cast<VulkanTextureData*>(renderTarget.nativeTexture());

    if (data == nullptr || !data->hasSampleableDepth())
        return;

    const auto sampler = getVulkanShared().getSampler(sampling);

    if (sampler == VK_NULL_HANDLE)
        return;

    impl->textures[slot] = {sampler, data->depthReadView, imageDepthSampled.layout};
    impl->boundTextures |= 1u << slot;
    impl->descriptorsDirty = true;
}

void RenderPass::setVertexStorageBuffer(const Buffer& buffer, int slot)
{
    impl->bindStorageBuffer(BufferRange::of(buffer), slot);
}

void RenderPass::setVertexStorageBuffer(const BufferRange& range, int slot)
{
    impl->bindStorageBuffer(range, slot);
}

void RenderPass::setFragmentStorageBuffer(const Buffer& buffer, int slot)
{
    impl->bindStorageBuffer(BufferRange::of(buffer), slot);
}

void RenderPass::setFragmentStorageBuffer(const BufferRange& range, int slot)
{
    impl->bindStorageBuffer(range, slot);
}

void RenderPass::setVertexBytes(const void* data, int bytes, int slot)
{
    impl->uploadUniforms(data, bytes, slot);
}

void RenderPass::setFragmentBytes(const void* data, int bytes, int slot)
{
    impl->uploadUniforms(data, bytes, slot);
}

void RenderPass::draw(int vertexCount, int firstVertex)
{
    drawInstanced(vertexCount, 1, firstVertex, 0);
}

void RenderPass::drawInstanced(int vertexCount,
                               int instanceCount,
                               int firstVertex,
                               int firstInstance)
{
    if (!impl->canRecord() || !impl->bindDescriptors())
        return;

    vkCmdDraw(impl->commandBuffer(),
              static_cast<std::uint32_t>(vertexCount),
              static_cast<std::uint32_t>(instanceCount),
              static_cast<std::uint32_t>(firstVertex),
              static_cast<std::uint32_t>(firstInstance));
}

void RenderPass::drawIndexed(const Buffer& indices,
                             int indexCount,
                             IndexFormat format,
                             int firstIndex,
                             int baseVertex)
{
    drawIndexed(
        BufferRange::of(indices), indexCount, format, firstIndex, baseVertex);
}

void RenderPass::drawIndexed(const BufferRange& indices,
                             int indexCount,
                             IndexFormat format,
                             int firstIndex,
                             int baseVertex)
{
    drawIndexedInstanced(indices, indexCount, 1, format, firstIndex, 0, baseVertex);
}

void RenderPass::drawIndexedInstanced(const Buffer& indices,
                                      int indexCount,
                                      int instanceCount,
                                      IndexFormat format,
                                      int firstIndex,
                                      int firstInstance,
                                      int baseVertex)
{
    drawIndexedInstanced(BufferRange::of(indices),
                         indexCount,
                         instanceCount,
                         format,
                         firstIndex,
                         firstInstance,
                         baseVertex);
}

void RenderPass::drawIndexedInstanced(const BufferRange& indices,
                                      int indexCount,
                                      int instanceCount,
                                      IndexFormat format,
                                      int firstIndex,
                                      int firstInstance,
                                      int baseVertex)
{
    if (!impl->canRecord() || !impl->bindIndexRange(indices, format)
        || !impl->bindDescriptors())
        return;

    vkCmdDrawIndexed(impl->commandBuffer(),
                     static_cast<std::uint32_t>(indexCount),
                     static_cast<std::uint32_t>(instanceCount),
                     static_cast<std::uint32_t>(firstIndex),
                     baseVertex,
                     static_cast<std::uint32_t>(firstInstance));
}

void RenderPass::end()
{
    // Before vkCmdEndRendering: a participant's queued draws are still draws.
    drainParticipants();

    if (!impl->encoder)
        return;

    endTimedPass(*impl->encoder);

    auto buffer = impl->commandBuffer();
    vkCmdEndRendering(buffer);

    impl->encoder->commands->context->setRenderPassOpen(false);

    // Barriers are legal only now that vkCmdEndRendering has run.
    if (auto* target = impl->encoder->target)
    {
        transitionTextureForUse(buffer, *target, target->restingUse());
        transitionDepthForUse(buffer, *target, target->depthRestingUse());
        transitionResolvedDepthForUse(buffer, *target, imageDepthSampled);
    }

    impl->encoder.reset();
}
} // namespace eacp::GPU

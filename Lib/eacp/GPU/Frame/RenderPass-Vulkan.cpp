#include "RenderPass.h"

#include "../Buffer/Buffer.h"
#include "../Pipeline/RenderPipeline.h"
#include "../Texture/Texture.h"
#include "../Vulkan/VulkanBackend-Linux.h"
#include "../Vulkan/VulkanTypes.h"

#include <algorithm>
#include <cmath>
#include <memory>

namespace eacp::GPU
{
namespace
{
struct VulkanRenderPassBackend final : RenderPassBackend
{
    VulkanRenderPassBackend(VulkanRenderEncoder* encoderToUse,
                            int width,
                            int height)
        : encoder(encoderToUse)
        , pixelWidth(width)
        , pixelHeight(height)
    {
    }

    ~VulkanRenderPassBackend() override { end(); }

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

    void setStorageBuffer(const BufferRange& range, int slot) override
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

    void setBytes(const void* data, int bytes, int slot) override
    {
        if (encoder == nullptr || bytes <= 0 || slot < 0 || slot >= maxUniformSlots)
            return;

        auto& commands = *encoder->commands;
        uniforms =
            commands.context->uploadConstants(commands, data, (std::size_t) bytes);
        descriptorsDirty = true;
    }

    std::unique_ptr<VulkanRenderEncoder> encoder;

    int pixelWidth = 0;
    int pixelHeight = 0;

    VkDescriptorBufferInfo buffers[maxBufferSlots] = {};
    std::uint32_t boundBuffers = 0;

    VkDescriptorImageInfo textures[maxTextureSlots] = {};
    std::uint32_t boundTextures = 0;

    ConstantRange uniforms;

    bool descriptorsDirty = true;
    int targetWidth() const override { return pixelWidth; }

    int targetHeight() const override { return pixelHeight; }

    void setScissorRect(const Graphics::Rect& rect) override
    {
        if (!encoder || pixelWidth <= 0 || pixelHeight <= 0)
            return;

        // Outward: rounding an edge inward would shave a column of coverage off
        // it.
        const auto left =
            std::clamp(static_cast<int>(std::floor(rect.x)), 0, pixelWidth);
        const auto top =
            std::clamp(static_cast<int>(std::floor(rect.y)), 0, pixelHeight);
        const auto right = std::clamp(
            static_cast<int>(std::ceil(rect.x + rect.w)), left, pixelWidth);
        const auto bottom = std::clamp(
            static_cast<int>(std::ceil(rect.y + rect.h)), top, pixelHeight);

        // Framebuffer coordinates, y down: the negative viewport height flips
        // clip space and nothing else.
        const VkRect2D scissor {{left, top},
                                {static_cast<std::uint32_t>(right - left),
                                 static_cast<std::uint32_t>(bottom - top)}};

        vkCmdSetScissor(commandBuffer(), 0, 1, &scissor);
    }

    void clearScissorRect() override
    {
        if (!encoder || pixelWidth <= 0 || pixelHeight <= 0)
            return;

        const VkRect2D scissor {{0, 0},
                                {static_cast<std::uint32_t>(pixelWidth),
                                 static_cast<std::uint32_t>(pixelHeight)}};

        vkCmdSetScissor(commandBuffer(), 0, 1, &scissor);
    }

    // The negative height answers Vulkan's y-down NDC; winding and every shader
    // in the tree depend on it.
    void setViewport(const Graphics::Rect& rect,
                     float nearDepth,
                     float farDepth) override
    {
        if (!encoder || pixelWidth <= 0 || pixelHeight <= 0)
            return;

        if (rect.w <= 0.f || rect.h <= 0.f || rect.x < 0.f || rect.y < 0.f
            || rect.x + rect.w > static_cast<float>(pixelWidth)
            || rect.y + rect.h > static_cast<float>(pixelHeight))
            return;

        const VkViewport viewport {
            rect.x, rect.y + rect.h, rect.w, -rect.h, nearDepth, farDepth};

        vkCmdSetViewport(commandBuffer(), 0, 1, &viewport);
    }

    void clearViewport() override
    {
        if (!encoder || pixelWidth <= 0 || pixelHeight <= 0)
            return;

        const VkViewport viewport {0.f,
                                   static_cast<float>(pixelHeight),
                                   static_cast<float>(pixelWidth),
                                   -static_cast<float>(pixelHeight),
                                   0.f,
                                   1.f};

        vkCmdSetViewport(commandBuffer(), 0, 1, &viewport);
    }

    void setPipeline(const RenderPipeline& pipeline) override
    {
        if (!encoder)
            return;

        auto* state = static_cast<VulkanRenderPipeline*>(pipeline.nativeState());

        encoder->pipeline = nullptr;

        if (state == nullptr || state->pipeline == VK_NULL_HANDLE)
            return;

        encoder->pipeline = state;

        vkCmdBindPipeline(
            commandBuffer(), VK_PIPELINE_BIND_POINT_GRAPHICS, state->pipeline);

        // The set that was bound belongs to the previous pipeline's layout.
        descriptorsDirty = true;
    }

    void setStencilReference(unsigned int value) override
    {
        if (!encoder)
            return;

        vkCmdSetStencilReference(commandBuffer(),
                                 VK_STENCIL_FACE_FRONT_AND_BACK,
                                 static_cast<std::uint32_t>(value));
    }

    void setVertexBuffer(const BufferRange& range, int index) override
    {
        if (!encoder || range.buffer == nullptr || index < 0)
            return;

        auto* data = static_cast<VulkanBufferData*>(range.buffer->nativeBuffer());

        if (data == nullptr || data->buffer == VK_NULL_HANDLE || range.offset < 0
            || static_cast<std::size_t>(range.offset) >= data->size)
            return;

        noteBufferUse(*encoder->commands, *data, bufferVertexRead);

        const auto offset = static_cast<VkDeviceSize>(range.offset);

        vkCmdBindVertexBuffers(commandBuffer(),
                               static_cast<std::uint32_t>(index),
                               1,
                               &data->buffer,
                               &offset);
    }

    // No barrier: Vulkan forbids one inside a render pass instance, and the
    // resting-layout rule on VulkanTextureData leaves the image ready to sample.
    void setFragmentTexture(const Texture& texture,
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

        textures[slot] = {sampler, data->sampledView, data->restingUse().layout};
        boundTextures |= 1u << slot;
        descriptorsDirty = true;
    }

    void setFragmentDepthTexture(const Texture& renderTarget,
                                 int slot,
                                 TextureSampling sampling) override
    {
        if (!encoder || slot < 0 || slot >= maxTextureSlots)
            return;

        auto* data = static_cast<VulkanTextureData*>(renderTarget.nativeTexture());

        if (data == nullptr || !data->hasSampleableDepth())
            return;

        const auto sampler = getVulkanShared().getSampler(sampling);

        if (sampler == VK_NULL_HANDLE)
            return;

        textures[slot] = {sampler, data->depthReadView, imageDepthSampled.layout};
        boundTextures |= 1u << slot;
        descriptorsDirty = true;
    }

    void drawInstanced(int vertexCount,
                       int instanceCount,
                       int firstVertex,
                       int firstInstance) override
    {
        if (!canRecord() || !bindDescriptors())
            return;

        vkCmdDraw(commandBuffer(),
                  static_cast<std::uint32_t>(vertexCount),
                  static_cast<std::uint32_t>(instanceCount),
                  static_cast<std::uint32_t>(firstVertex),
                  static_cast<std::uint32_t>(firstInstance));
    }

    void drawIndexedInstanced(const BufferRange& indices,
                              int indexCount,
                              int instanceCount,
                              IndexFormat format,
                              int firstIndex,
                              int firstInstance,
                              int baseVertex) override
    {
        if (!canRecord() || !bindIndexRange(indices, format) || !bindDescriptors())
            return;

        vkCmdDrawIndexed(commandBuffer(),
                         static_cast<std::uint32_t>(indexCount),
                         static_cast<std::uint32_t>(instanceCount),
                         static_cast<std::uint32_t>(firstIndex),
                         baseVertex,
                         static_cast<std::uint32_t>(firstInstance));
    }

    void end() override
    {
        if (!encoder)
            return;

        endTimedPass(*encoder);

        auto buffer = commandBuffer();
        vkCmdEndRendering(buffer);

        encoder->commands->context->setRenderPassOpen(false);

        // Barriers are legal only now that vkCmdEndRendering has run.
        if (auto* target = encoder->target)
        {
            transitionTextureForUse(buffer, *target, target->restingUse());
            transitionDepthForUse(buffer, *target, target->depthRestingUse());
            transitionResolvedDepthForUse(buffer, *target, imageDepthSampled);
        }

        encoder.reset();
    }
};
} // namespace

std::unique_ptr<RenderPassBackend> makeVulkanRenderPass(VulkanRenderEncoder* encoder,
                                                        int targetWidth,
                                                        int targetHeight)
{
    return std::make_unique<VulkanRenderPassBackend>(
        encoder, targetWidth, targetHeight);
}
} // namespace eacp::GPU

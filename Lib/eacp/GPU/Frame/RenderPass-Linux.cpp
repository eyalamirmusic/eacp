#include "RenderPass.h"

#include "IndirectDrawPlan.h"

#include "../Buffer/Buffer.h"
#include "../Pipeline/RenderPipeline.h"
#include "../Texture/Sampler.h"
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

        VkWriteDescriptorSet writes[maxBufferSlots + maxStageTextureSlots + 1] = {};
        auto writeCount = std::uint32_t {0};

        for (auto slot = 0; slot < maxStageTextureSlots; ++slot)
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

    // A combined image sampler at the texture's binding: the sampler travels in
    // the descriptor write, whichever overload picked it.
    void bindTexture(int slot,
                     VkSampler sampler,
                     VkImageView view,
                     VkImageLayout layout)
    {
        textures[slot] = {sampler, view, layout};
        boundTextures |= 1u << slot;
        descriptorsDirty = true;
    }

    void bindColorTexture(const Texture& texture, int slot, VkSampler sampler)
    {
        if (!encoder || slot < 0 || slot >= maxStageTextureSlots
            || sampler == VK_NULL_HANDLE)
            return;

        auto* data = static_cast<VulkanTextureData*>(texture.nativeTexture());

        if (data == nullptr || !data->isValid())
            return;

        bindTexture(slot, sampler, data->sampledView, data->restingUse().layout);
    }

    // The whole plane at slice -1, one slice's 2D view otherwise.
    void bindDepthTexture(const Texture& renderTarget,
                          int slot,
                          VkSampler sampler,
                          int slice = -1)
    {
        if (!encoder || slot < 0 || slot >= maxStageTextureSlots
            || sampler == VK_NULL_HANDLE)
            return;

        auto* data = static_cast<VulkanTextureData*>(renderTarget.nativeTexture());

        if (data == nullptr || !data->hasSampleableDepth())
            return;

        const auto view =
            slice < 0 ? data->depthReadView : vulkanDepthSliceView(*data, slice);

        if (view == VK_NULL_HANDLE)
            return;

        bindTexture(slot, sampler, view, imageDepthSampled.layout);
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

    VulkanBufferData* noteIndirectRead(const Buffer& buffer)
    {
        auto* data = static_cast<VulkanBufferData*>(buffer.nativeBuffer());

        if (data == nullptr || data->buffer == VK_NULL_HANDLE)
            return nullptr;

        noteBufferUse(*encoder->commands, *data, bufferIndirectRead);
        return data;
    }

    // One call for the whole run where the device has multiDrawIndirect, in
    // runs no longer than its maxDrawIndirectCount; one call per record where it
    // does not.
    template <typename Record>
    void recordIndirectRuns(const IndirectDrawPlan& plan, Record&& record)
    {
        auto& shared = getVulkanShared();
        const auto runLimit =
            shared.supportsMultiDrawIndirect()
                ? std::max(1,
                           static_cast<int>(std::min(
                               shared.getProperties().limits.maxDrawIndirectCount,
                               static_cast<std::uint32_t>(plan.drawCount))))
                : 1;

        for (auto first = 0; first < plan.drawCount; first += runLimit)
        {
            const auto count = std::min(runLimit, plan.drawCount - first);
            const auto offset = plan.offset + first * plan.stride;

            record(static_cast<VkDeviceSize>(offset),
                   static_cast<std::uint32_t>(count),
                   static_cast<std::uint32_t>(plan.stride));
        }
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

    VkDescriptorImageInfo textures[maxStageTextureSlots] = {};
    std::uint32_t boundTextures = 0;

    ConstantRange uniforms;

    bool descriptorsDirty = true;
};

RenderPass::RenderPass(void* encoder,
                       int targetWidth,
                       int targetHeight,
                       OcclusionQueries* queries)
    : occlusionQueries(queries)
    , impl(encoder, targetWidth, targetHeight)
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
    impl->bindColorTexture(texture, slot, getVulkanShared().getSampler(sampling));
}

void RenderPass::setFragmentTexture(const Texture& texture,
                                    int slot,
                                    const Sampler& sampler)
{
    impl->bindColorTexture(texture, slot, toVkSampler(sampler));
}

void RenderPass::setFragmentDepthTexture(const Texture& renderTarget,
                                         int slot,
                                         TextureSampling sampling)
{
    impl->bindDepthTexture(
        renderTarget, slot, getVulkanShared().getSampler(sampling));
}

void RenderPass::setFragmentDepthTexture(const Texture& renderTarget,
                                         int slot,
                                         const Sampler& sampler)
{
    impl->bindDepthTexture(renderTarget, slot, toVkSampler(sampler));
}

void RenderPass::setFragmentDepthTextureSlice(const Texture& renderTarget,
                                              int slice,
                                              int slot,
                                              TextureSampling sampling)
{
    impl->bindDepthTexture(
        renderTarget, slot, getVulkanShared().getSampler(sampling), slice);
}

void RenderPass::setFragmentDepthTextureSlice(const Texture& renderTarget,
                                              int slice,
                                              int slot,
                                              const Sampler& sampler)
{
    impl->bindDepthTexture(renderTarget, slot, toVkSampler(sampler), slice);
}

// The layout declares every combined image sampler for both stages, so the
// vertex binds land on the same binding as the fragment ones.
void RenderPass::setVertexTexture(const Texture& texture,
                                  int slot,
                                  TextureSampling sampling)
{
    setFragmentTexture(texture, slot, sampling);
}

void RenderPass::setVertexTexture(const Texture& texture,
                                  int slot,
                                  const Sampler& sampler)
{
    setFragmentTexture(texture, slot, sampler);
}

void RenderPass::setVertexDepthTexture(const Texture& renderTarget,
                                       int slot,
                                       TextureSampling sampling)
{
    setFragmentDepthTexture(renderTarget, slot, sampling);
}

void RenderPass::setVertexDepthTexture(const Texture& renderTarget,
                                       int slot,
                                       const Sampler& sampler)
{
    setFragmentDepthTexture(renderTarget, slot, sampler);
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

void RenderPass::drawIndirect(const Buffer& arguments,
                              std::int64_t offset,
                              int drawCount,
                              std::int64_t stride)
{
    const auto plan = planIndirectDraws(
        arguments, offset, drawCount, stride, sizeof(DrawArguments));

    if (!plan || !impl->canRecord())
        return;

    auto* data = impl->noteIndirectRead(arguments);

    if (data == nullptr || !impl->bindDescriptors())
        return;

    impl->recordIndirectRuns(
        *plan,
        [&](VkDeviceSize first, std::uint32_t count, std::uint32_t step)
        {
            vkCmdDrawIndirect(
                impl->commandBuffer(), data->buffer, first, count, step);
        });
}

void RenderPass::drawIndirectCount(const Buffer& arguments,
                                   std::int64_t offset,
                                   const Buffer& count,
                                   std::int64_t countOffset,
                                   int maxDrawCount,
                                   std::int64_t stride)
{
    if (!getVulkanShared().supportsIndirectCount())
    {
        traceIndirectCountUnsupported();
        return;
    }

    const auto plan = planIndirectDraws(
        arguments, offset, maxDrawCount, stride, sizeof(DrawArguments));

    if (!plan || !isValidDrawCountBuffer(count, countOffset) || !impl->canRecord())
        return;

    auto* data = impl->noteIndirectRead(arguments);
    auto* countData = impl->noteIndirectRead(count);

    if (data == nullptr || countData == nullptr || !impl->bindDescriptors())
        return;

    vkCmdDrawIndirectCount(impl->commandBuffer(),
                           data->buffer,
                           static_cast<VkDeviceSize>(plan->offset),
                           countData->buffer,
                           static_cast<VkDeviceSize>(countOffset),
                           static_cast<std::uint32_t>(plan->drawCount),
                           static_cast<std::uint32_t>(plan->stride));
}

void RenderPass::drawIndexedIndirect(const BufferRange& indices,
                                     IndexFormat format,
                                     const Buffer& arguments,
                                     std::int64_t offset,
                                     int drawCount,
                                     std::int64_t stride)
{
    const auto plan = planIndirectDraws(
        arguments, offset, drawCount, stride, sizeof(DrawIndexedArguments));

    if (!plan || !impl->canRecord())
        return;

    auto* data = impl->noteIndirectRead(arguments);

    if (data == nullptr || !impl->bindIndexRange(indices, format)
        || !impl->bindDescriptors())
        return;

    impl->recordIndirectRuns(
        *plan,
        [&](VkDeviceSize first, std::uint32_t count, std::uint32_t step)
        {
            vkCmdDrawIndexedIndirect(
                impl->commandBuffer(), data->buffer, first, count, step);
        });
}

void RenderPass::drawIndexedIndirectCount(const BufferRange& indices,
                                          IndexFormat format,
                                          const Buffer& arguments,
                                          std::int64_t offset,
                                          const Buffer& count,
                                          std::int64_t countOffset,
                                          int maxDrawCount,
                                          std::int64_t stride)
{
    if (!getVulkanShared().supportsIndirectCount())
    {
        traceIndirectCountUnsupported();
        return;
    }

    const auto plan = planIndirectDraws(
        arguments, offset, maxDrawCount, stride, sizeof(DrawIndexedArguments));

    if (!plan || !isValidDrawCountBuffer(count, countOffset) || !impl->canRecord())
        return;

    auto* data = impl->noteIndirectRead(arguments);
    auto* countData = impl->noteIndirectRead(count);

    if (data == nullptr || countData == nullptr
        || !impl->bindIndexRange(indices, format) || !impl->bindDescriptors())
        return;

    vkCmdDrawIndexedIndirectCount(impl->commandBuffer(),
                                  data->buffer,
                                  static_cast<VkDeviceSize>(plan->offset),
                                  countData->buffer,
                                  static_cast<VkDeviceSize>(countOffset),
                                  static_cast<std::uint32_t>(plan->drawCount),
                                  static_cast<std::uint32_t>(plan->stride));
}

// PRECISE for a count where the device has occlusionQueryPrecise, which was
// enabled with it; without the bit a query promises only zero or not zero,
// which is all AnySamples asks.
void RenderPass::nativeBeginOcclusionQuery(int index, OcclusionMode mode)
{
    auto pool = static_cast<VkQueryPool>(occlusionQueries->nativeQueries());

    if (!impl->encoder || pool == VK_NULL_HANDLE)
        return;

    const auto precise = mode == OcclusionMode::SampleCount
                         && getVulkanShared().supportsPreciseOcclusionQueries();

    vkCmdBeginQuery(impl->commandBuffer(),
                    pool,
                    static_cast<std::uint32_t>(index),
                    precise ? VK_QUERY_CONTROL_PRECISE_BIT : 0);
}

void RenderPass::nativeEndOcclusionQuery(int index, OcclusionMode)
{
    auto pool = static_cast<VkQueryPool>(occlusionQueries->nativeQueries());

    if (impl->encoder && pool != VK_NULL_HANDLE)
        vkCmdEndQuery(
            impl->commandBuffer(), pool, static_cast<std::uint32_t>(index));
}

// Once rendering has ended: each used query copied into its eight bytes of the
// set's host buffer, waiting on its availability, and then reset so the next
// pass or frame may begin it - query commands on one pool execute in order, so
// the reset lands after the copy has read it. The barrier orders this pass's
// copies after an earlier pass's into the same bytes.
void RenderPass::resolveOcclusionQueries()
{
    if (occlusionQueries == nullptr || usedQueries.empty() || !impl->encoder)
        return;

    auto pool = static_cast<VkQueryPool>(occlusionQueries->nativeQueries());
    auto results = static_cast<VkBuffer>(occlusionQueries->nativeResults());

    if (pool == VK_NULL_HANDLE || results == VK_NULL_HANDLE)
        return;

    auto buffer = impl->commandBuffer();

    VkMemoryBarrier2 barrier = {};
    barrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2;
    barrier.srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
    barrier.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    barrier.dstStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
    barrier.dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;

    VkDependencyInfo dependency = {};
    dependency.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
    dependency.memoryBarrierCount = 1;
    dependency.pMemoryBarriers = &barrier;

    vkCmdPipelineBarrier2(buffer, &dependency);

    for (const auto& used: usedQueries)
    {
        const auto query = static_cast<std::uint32_t>(used.index);

        vkCmdCopyQueryPoolResults(
            buffer,
            pool,
            query,
            1,
            results,
            static_cast<VkDeviceSize>(used.index) * sizeof(std::uint64_t),
            sizeof(std::uint64_t),
            VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT);

        vkCmdResetQueryPool(buffer, pool, query, 1);
    }
}

void RenderPass::end()
{
    // Before vkCmdEndRendering: a participant's queued draws are still draws.
    drainParticipants();

    if (!impl->encoder)
        return;

    // After them, so a participant's draws inside an open query count; a query
    // may not stay active past the end of rendering.
    endOcclusionQuery();

    endTimedPass(*impl->encoder);

    auto buffer = impl->commandBuffer();

    if (getVulkanShared().usesRenderPasses())
        vkCmdEndRenderPass(buffer);
    else
        vkCmdEndRendering(buffer);

    impl->encoder->commands->context->setRenderPassOpen(false);

    // Outside rendering, where a copy and a reset are legal.
    resolveOcclusionQueries();

    // Barriers are legal only now that vkCmdEndRendering has run.
    for (auto* target: impl->encoder->targets)
        transitionTextureForUse(buffer, *target, target->restingUse());

    if (auto* depth = impl->encoder->depthTarget)
    {
        transitionDepthForUse(buffer, *depth, depth->depthRestingUse());
        transitionResolvedDepthForUse(buffer, *depth, imageDepthSampled);
    }

    impl->encoder.reset();
}

void RenderPass::setVertexDepthTextureSlice(const Texture& renderTarget,
                                            int slice,
                                            int slot,
                                            TextureSampling sampling)
{
    setFragmentDepthTextureSlice(renderTarget, slice, slot, sampling);
}

void RenderPass::setVertexDepthTextureSlice(const Texture& renderTarget,
                                            int slice,
                                            int slot,
                                            const Sampler& sampler)
{
    setFragmentDepthTextureSlice(renderTarget, slice, slot, sampler);
}
} // namespace eacp::GPU

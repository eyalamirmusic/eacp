#include "RenderPass.h"

#include "../Buffer/Buffer.h"
#include "../Pipeline/RenderPipeline.h"
#include "../Texture/Texture.h"
#include "../Vulkan/VulkanTypes.h"

#include <algorithm>
#include <cmath>
#include <memory>

// Linux/Vulkan backend. Records draws onto the frame's recording through the
// VulkanRenderEncoder, which the Frame handed over already inside a
// vkCmdBeginRendering. The encoder is owned here so it goes when the pass goes;
// the CommandContext behind it stays the Frame's, which submits it.
//
// **Nothing here records a barrier, and that is a rule rather than an
// optimisation.** Vulkan forbids buffer barriers inside a render pass instance
// and allows only a degenerate kind of image barrier, so a bind that needed one
// could not have it. What makes the binds sound is the pair the pass is
// bracketed by: the frame records one global barrier before vkCmdBeginRendering
// (barrierBeforeRendering), covering every upload and every dispatch this
// recording has made against everything a draw can read; and every image the
// pass could bind is already in the layout its descriptor names, because the
// resting-layout rule on VulkanTextureData puts it there - an upload leaves a
// texture sampleable, a kernel's output rests in GENERAL which a sampler reads
// perfectly well, and a render target is put back by the pass that drew it. So
// setFragmentTexture writes a descriptor and nothing else.
//
// **Binds are collected, not recorded.** Vulkan has one binding model for
// everything - a descriptor set, allocated out of the recording's pool - so a
// bind is a descriptor write, and a write to a set that is already bound is a
// write the draw may or may not see. The set is therefore built at the draw,
// and rebuilt whenever anything has been bound since the last one. A set per
// draw would be affordable (the pool is reset with the recording), so the dirty
// flag is only there to keep a loop that binds once and draws many times from
// paying for it.
//
// The uniform block is the one binding that is not a fresh descriptor: it is a
// UNIFORM_BUFFER_DYNAMIC over the recording's constant page, so consecutive
// draws differ in the dynamic offset alone.

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

    // Everything bound since the last draw, written into a fresh set and bound.
    // False when the set could not be had, which stops the draw rather than
    // running it against whatever was bound before.
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
            // Offset zero and the dynamic offset carrying the whole of it: the
            // page is one buffer for the recording, and where in it this block
            // sits is exactly what a dynamic offset is for.
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

        // One dynamic offset always, whether or not a uniform block was bound:
        // the layout declares one dynamic descriptor and the count has to match
        // it. Zero is a legal offset into a page nothing reads.
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

    // The index buffer bound for a draw, with the tracking stamped so a kernel
    // that later writes this buffer in the same recording barriers from the
    // index read rather than from whatever preceded it. False when there is
    // nothing to bind, so the draw can be skipped rather than issued over
    // nothing.
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

    // Both storage-buffer setters, which differ on this backend by nothing at
    // all: one GLSL std430 block is one binding read by whichever stage
    // declared it, and the render set layout makes every binding visible to
    // both. See VulkanRenderPipeline.
    void bindStorageBuffer(const Buffer& buffer, int slot)
    {
        if (encoder == nullptr || slot < 0 || slot >= maxBufferSlots)
            return;

        auto* data = static_cast<VulkanBufferData*>(buffer.nativeBuffer());

        if (data == nullptr || data->buffer == VK_NULL_HANDLE)
            return;

        noteBufferUse(*encoder->commands, *data, bufferGraphicsRead);

        buffers[slot] = {data->buffer, 0, VK_WHOLE_SIZE};
        boundBuffers |= 1u << slot;
        descriptorsDirty = true;
    }

    // The uniform block, which both stages read from one binding. setVertexBytes
    // and setFragmentBytes therefore land here and the last one wins - safe
    // because RenderPass::setUniforms, the only caller in the tree that makes
    // both calls, hands the same packed block to each. See the note on
    // VulkanRenderPipeline for what a caller that wanted two different blocks
    // would need first.
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

    // Render target size in pixels, for clamping scissor rects. Reported even
    // where there is no encoder, because it is what the caller passed in rather
    // than anything about a pass.
    int targetWidth = 0;
    int targetHeight = 0;

    VkDescriptorBufferInfo buffers[maxBufferSlots] = {};
    std::uint32_t boundBuffers = 0;

    VkDescriptorImageInfo textures[maxTextureSlots] = {};
    std::uint32_t boundTextures = 0;

    ConstantRange uniforms;

    // Whether anything has been bound since the set that is currently bound was
    // written. True to begin with, so the first draw always gets a set.
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

    // Round outward before clamping: rounding a scrolled region's edge inward
    // would shave a column of glyph coverage off the boundary.
    const auto left =
        std::clamp(static_cast<int>(std::floor(rect.x)), 0, impl->targetWidth);
    const auto top =
        std::clamp(static_cast<int>(std::floor(rect.y)), 0, impl->targetHeight);
    const auto right = std::clamp(
        static_cast<int>(std::ceil(rect.x + rect.w)), left, impl->targetWidth);
    const auto bottom = std::clamp(
        static_cast<int>(std::ceil(rect.y + rect.h)), top, impl->targetHeight);

    // The scissor is in framebuffer coordinates with y down, which is what
    // Graphics::Rect already is - the negative viewport height flips clip space
    // and nothing else, so this is the one place on the backend that needs no
    // adjustment for it.
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

// The negative height is the whole of Vulkan's coordinate difference from the
// other two backends: NDC y points down here, and flipping the viewport rather
// than the projection or gl_Position is what leaves winding, depth range and
// every shader in the tree meaning what they mean on Metal. See plan.md §3.5.
void RenderPass::setViewport(const Graphics::Rect& rect,
                             float nearDepth,
                             float farDepth)
{
    if (!impl->encoder || impl->targetWidth <= 0 || impl->targetHeight <= 0)
        return;

    // No rounding, unlike the scissor: a viewport is a float rectangle in every
    // one of the three APIs, and rounding it would move the mapping rather than
    // the clip. Rejected rather than clamped when it leaves the target, for the
    // reason the header gives - a clamped viewport keeps drawing and silently
    // squashes the picture, and nothing appearing is easier to find.
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

// Topology, cull mode and front face are all baked into the VkPipeline here -
// only the viewport, the scissor and the stencil reference are dynamic - so
// there is nothing to apply beside the bind, where Metal has to re-apply two
// pieces of encoder state at every setPipeline. See makeRasterizationState.
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

    // The set that was bound belongs to the previous pipeline's layout. Every
    // render pipeline on this backend shares one layout, so it would in fact
    // still be bound - but saying so here would make the pass depend on that,
    // and a second layout would then break draws silently rather than loudly.
    impl->descriptorsDirty = true;
}

void RenderPass::setStencilReference(unsigned int value)
{
    if (!impl->encoder)
        return;

    // Both faces from one value, which is what the eacp API says a reference is
    // - RenderPipelineDescriptor carries one pair of masks for the same reason.
    vkCmdSetStencilReference(impl->commandBuffer(),
                             VK_STENCIL_FACE_FRONT_AND_BACK,
                             static_cast<std::uint32_t>(value));
}

void RenderPass::setVertexBuffer(const Buffer& buffer, int index)
{
    setVertexBuffer(BufferRange::of(buffer), index);
}

// The stride is not passed here as it is on the other two backends: it lives in
// the pipeline's VkVertexInputBindingDescription, which is why
// VulkanRenderPipeline::strides exists for RenderPipeline to build the pipeline
// from and nothing reads it back at bind time. The offset is the range's whole
// contribution - vertex zero of the draw is the byte at the offset.
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

// A combined image sampler: GLSL has no separate sampler declaration, so the
// sampler the *shader* asked for travels with the image in the one descriptor.
// See TextureSampling, which is why the sampling is an argument rather than a
// property of the texture.
//
// No barrier, and nothing to move. Whatever is bound here is already in the
// layout this names: an upload left the image at its resting layout, a kernel's
// output rests in the GENERAL a sampler reads, and a render target was put back
// by the pass that drew it (RenderPass::end). That is the resting-layout rule
// on VulkanTextureData, and it exists because this function has no way to
// record a barrier at all.
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

// The depth plane of a render target, on the same slot space and with the same
// sampling rule. The view is the depth-only one over whichever image
// sampledDepthImage() names - the resolve on a multisampled target and the
// attachment itself on every other - and it rests in
// DEPTH_STENCIL_READ_ONLY_OPTIMAL, which is where the pass that drew it left it.
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
    impl->bindStorageBuffer(buffer, slot);
}

void RenderPass::setFragmentStorageBuffer(const Buffer& buffer, int slot)
{
    impl->bindStorageBuffer(buffer, slot);
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

    // baseVertex is added to every index before the vertex is fetched, which is
    // the whole of what it means on all three backends; firstIndex counts from
    // the range's own index zero, the bind having started at its offset.
    vkCmdDrawIndexed(impl->commandBuffer(),
                     static_cast<std::uint32_t>(indexCount),
                     static_cast<std::uint32_t>(instanceCount),
                     static_cast<std::uint32_t>(firstIndex),
                     baseVertex,
                     static_cast<std::uint32_t>(firstInstance));
}

void RenderPass::end()
{
    // Before anything closes, so a batching renderer's queued draws still land
    // on this pass. See RenderPass::Participant - and note that this comes
    // first even though the transitions below are the last thing recorded: a
    // participant draws, and a draw after vkCmdEndRendering is not a draw.
    drainParticipants();

    if (!impl->encoder)
        return;

    // After the participants, so what they flushed is inside the pass being
    // timed rather than after it.
    endTimedPass(*impl->encoder);

    auto buffer = impl->commandBuffer();
    vkCmdEndRendering(buffer);

    // Uploads may join this recording again, which they could not while the
    // render pass instance was running. See VulkanContext::getRecordingForCopy.
    impl->encoder->commands->context->setRenderPassOpen(false);

    // And now barriers are legal again, which is the only reason this is here
    // rather than at the frame's end: the attachments go back to where a pass
    // can find them, so the next thing to bind, sample or read one does not
    // have to know what was drawn.
    //
    // The multisampled companion is deliberately left where it is - it is
    // created COLOR_ATTACHMENT-only, so there is no sampleable layout for it to
    // rest in, and what orders two passes over it is barrierBeforeRendering.
    if (auto* target = impl->encoder->target)
    {
        transitionTextureForUse(buffer, *target, target->restingUse());
        transitionDepthForUse(buffer, *target, target->depthRestingUse());
        transitionResolvedDepthForUse(buffer, *target, imageDepthSampled);
    }

    // Commands were recorded onto the frame's command buffer, which submits
    // when the frame is destroyed; releasing the encoder marks the pass
    // finished and makes a second end() a no-op.
    impl->encoder.reset();
}
} // namespace eacp::GPU

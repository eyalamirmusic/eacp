#include "RenderPass.h"

#include "../Buffer/Buffer.h"
#include "../Pipeline/RenderPipeline.h"
#include "../Texture/Texture.h"
#include "../WebGPU/WebGPUTypes.h"

#include <algorithm>
#include <cmath>
#include <memory>

namespace eacp::GPU
{
struct RenderPass::Native
{
    Native(void* encoderHandle, int width, int height)
        : encoder(static_cast<WebRenderEncoder*>(encoderHandle))
        , targetWidth(width)
        , targetHeight(height)
    {
    }

    WGPURenderPassEncoder pass() const { return encoder->pass; }

    bool canRecord() const
    {
        return encoder != nullptr && encoder->pipeline != nullptr && pipelineBound;
    }

    // Binds are collected and written here, at the draw, as one group; a new
    // uniform block alone moves the dynamic offset and keeps the group.
    bool bindGroup()
    {
        auto& pipeline = *encoder->pipeline;
        const auto& layout = pipeline.bindings;

        if (layout.entries.empty())
            return true;

        const auto page = bound.uniforms.buffer;
        const auto size = bound.uniforms.size;

        if (page != groupUniformPage || size != groupUniformSize)
            groupDirty = true;

        if (groupDirty)
        {
            auto group = makeWebBindGroup(layout, pipeline.groupLayout, bound);

            if (group == nullptr)
                return false;

            releaseGroup();
            currentGroup = group;
            groupDirty = false;
            groupUniformPage = page;
            groupUniformSize = size;
        }

        const auto offset = webUniformOffset(layout, bound);

        wgpuRenderPassEncoderSetBindGroup(
            pass(), 0, currentGroup, layout.hasUniforms() ? 1 : 0, &offset);

        return true;
    }

    void releaseGroup()
    {
        if (currentGroup != nullptr)
            wgpuBindGroupRelease(currentGroup);

        currentGroup = nullptr;
    }

    bool bindIndexRange(const BufferRange& indices, IndexFormat format)
    {
        if (indices.buffer == nullptr)
            return false;

        auto* data = static_cast<WebBufferData*>(indices.buffer->nativeBuffer());

        if (data == nullptr || data->buffer == nullptr || indices.offset < 0
            || static_cast<std::size_t>(indices.offset) >= data->size)
            return false;

        const auto offset = static_cast<std::uint64_t>(indices.offset);

        wgpuRenderPassEncoderSetIndexBuffer(pass(),
                                            data->buffer,
                                            format == IndexFormat::UInt16
                                                ? WGPUIndexFormat_Uint16
                                                : WGPUIndexFormat_Uint32,
                                            offset,
                                            data->allocated - offset);
        return true;
    }

    void bindStorageBuffer(const BufferRange& range, int slot)
    {
        if (encoder == nullptr || slot < 0 || slot >= webMaxBufferSlots)
            return;

        const auto bind = webStorageBufferBind(range);

        if (!bind.isValid())
            return;

        bound.buffers[slot] = bind;
        groupDirty = true;
    }

    void bindTexture(WGPUTextureView view, int slot, TextureSampling sampling)
    {
        if (view == nullptr)
            return;

        bound.textures[slot] = {view, sampling};
        groupDirty = true;
    }

    // One block, bound to both stages, as on Vulkan.
    void uploadUniforms(const void* data, int bytes, int slot)
    {
        if (encoder == nullptr || bytes <= 0 || slot != 0)
            return;

        auto& context = *encoder->context;
        bound.uniforms =
            context.uploadUniforms(data, static_cast<std::size_t>(bytes));
    }

    ~Native() { releaseGroup(); }

    std::unique_ptr<WebRenderEncoder> encoder;

    int targetWidth = 0;
    int targetHeight = 0;

    WebBoundResources bound;
    WGPUBindGroup currentGroup = nullptr;
    WGPUBuffer groupUniformPage = nullptr;
    std::uint32_t groupUniformSize = 0;
    bool groupDirty = true;
    bool pipelineBound = false;
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

    wgpuRenderPassEncoderSetScissorRect(impl->pass(),
                                        static_cast<std::uint32_t>(left),
                                        static_cast<std::uint32_t>(top),
                                        static_cast<std::uint32_t>(right - left),
                                        static_cast<std::uint32_t>(bottom - top));
}

void RenderPass::clearScissorRect()
{
    if (!impl->encoder || impl->targetWidth <= 0 || impl->targetHeight <= 0)
        return;

    wgpuRenderPassEncoderSetScissorRect(
        impl->pass(),
        0,
        0,
        static_cast<std::uint32_t>(impl->targetWidth),
        static_cast<std::uint32_t>(impl->targetHeight));
}

// Clip-space y up and the framebuffer's down, as on Metal: nothing to flip.
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

    wgpuRenderPassEncoderSetViewport(
        impl->pass(), rect.x, rect.y, rect.w, rect.h, nearDepth, farDepth);
}

void RenderPass::clearViewport()
{
    if (!impl->encoder || impl->targetWidth <= 0 || impl->targetHeight <= 0)
        return;

    wgpuRenderPassEncoderSetViewport(impl->pass(),
                                     0.f,
                                     0.f,
                                     static_cast<float>(impl->targetWidth),
                                     static_cast<float>(impl->targetHeight),
                                     0.f,
                                     1.f);
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

    auto* state = static_cast<WebRenderPipeline*>(pipeline.nativeState());

    impl->pipelineBound = false;

    if (state == nullptr || !state->isValid())
        return;

    auto native = state->forDepthFormat(impl->encoder->depthFormat);

    if (native == nullptr)
        return;

    // The group that was bound belongs to the previous pipeline's layout.
    if (impl->encoder->pipeline != state)
        impl->groupDirty = true;

    impl->encoder->pipeline = state;
    impl->pipelineBound = true;

    wgpuRenderPassEncoderSetPipeline(impl->pass(), native);
}

void RenderPass::setStencilReference(unsigned int value)
{
    if (!impl->encoder)
        return;

    wgpuRenderPassEncoderSetStencilReference(impl->pass(),
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

    auto* data = static_cast<WebBufferData*>(range.buffer->nativeBuffer());

    if (data == nullptr || data->buffer == nullptr || range.offset < 0
        || static_cast<std::size_t>(range.offset) >= data->size)
        return;

    const auto offset = static_cast<std::uint64_t>(range.offset);

    wgpuRenderPassEncoderSetVertexBuffer(impl->pass(),
                                         static_cast<std::uint32_t>(index),
                                         data->buffer,
                                         offset,
                                         data->allocated - offset);
}

void RenderPass::setFragmentTexture(const Texture& texture,
                                    int slot,
                                    TextureSampling sampling)
{
    if (!impl->encoder || slot < 0 || slot >= maxTextureSlots)
        return;

    auto* data = static_cast<WebTextureData*>(texture.nativeTexture());

    if (data == nullptr || !data->isValid())
        return;

    impl->bindTexture(data->sampledView, slot, sampling);
}

void RenderPass::setFragmentDepthTexture(const Texture& renderTarget,
                                         int slot,
                                         TextureSampling sampling)
{
    if (!impl->encoder || slot < 0 || slot >= maxTextureSlots)
        return;

    auto* data = static_cast<WebTextureData*>(renderTarget.nativeTexture());

    if (data == nullptr || !data->hasSampleableDepth())
        return;

    impl->bindTexture(data->depthReadView, slot, sampling);
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
    if (!impl->canRecord() || vertexCount <= 0 || instanceCount <= 0
        || !impl->bindGroup())
        return;

    wgpuRenderPassEncoderDraw(impl->pass(),
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
    if (!impl->canRecord() || indexCount <= 0 || instanceCount <= 0
        || !impl->bindIndexRange(indices, format) || !impl->bindGroup())
        return;

    wgpuRenderPassEncoderDrawIndexed(impl->pass(),
                                     static_cast<std::uint32_t>(indexCount),
                                     static_cast<std::uint32_t>(instanceCount),
                                     static_cast<std::uint32_t>(firstIndex),
                                     baseVertex,
                                     static_cast<std::uint32_t>(firstInstance));
}

void RenderPass::end()
{
    // Before the encoder ends: a participant's queued draws are still draws.
    drainParticipants();

    if (!impl->encoder)
        return;

    wgpuRenderPassEncoderEnd(impl->pass());
    wgpuRenderPassEncoderRelease(impl->pass());

    impl->releaseGroup();
    impl->encoder.reset();
}
} // namespace eacp::GPU

#include "RenderPass.h"

#include "IndirectDrawPlan.h"

#include "../Buffer/Buffer.h"
#include "../Codegen/ShaderBindings.h"
#include "../Pipeline/RenderPipeline.h"
#include "../Texture/Sampler.h"
#include "../Texture/Texture.h"
#include "../WebGPU/WebGPUContext.h"

#include <algorithm>
#include <cmath>

namespace eacp::GPU
{
namespace
{
bool isFiltering(const SamplerDescriptor& descriptor)
{
    return descriptor.minFilter == SamplerFilter::Linear
           || descriptor.magFilter == SamplerFilter::Linear
           || descriptor.mipFilter == SamplerMipFilter::Linear;
}

bool isFiltering(TextureSampling sampling)
{
    return sampling.filter == TextureFilter::Linear;
}

wgpu::Sampler nativeOf(const Sampler& sampler)
{
    return wgpu::Sampler {static_cast<WGPUSampler>(sampler.nativeSampler())};
}

WebGPUBufferData* bufferDataOf(const Buffer& buffer)
{
    auto* data = static_cast<WebGPUBufferData*>(buffer.nativeBuffer());
    return data != nullptr && data->buffer != nullptr ? data : nullptr;
}
} // namespace

struct RenderPass::Native
{
    Native(void* encoderHandle, int width, int height)
        : encoder(static_cast<WebGPURenderEncoder*>(encoderHandle))
        , targetWidth(width)
        , targetHeight(height)
    {
    }

    bool canRecord() const { return encoder != nullptr && pipeline != nullptr; }

    WebGPUContext* context() const
    {
        return encoder != nullptr ? encoder->context : nullptr;
    }

    bool isTextureSlot(int slot) const
    {
        return encoder != nullptr && slot >= 0 && slot < maxStageTextureSlots;
    }

    void bindTexture(const wgpu::TextureView& view,
                     int slot,
                     const wgpu::Sampler& sampler,
                     bool filtering,
                     bool filterable = true)
    {
        if (!isTextureSlot(slot) || view == nullptr)
            return;

        bindings.bindView(webgpuTextureBinding(slot), view, filterable);
        bindings.bindSampler(webgpuSamplerBinding(slot), sampler, filtering);
    }

    void bindColorTexture(const Texture& texture,
                          int slot,
                          const wgpu::Sampler& sampler,
                          bool filtering)
    {
        auto* data = static_cast<WebGPUTextureData*>(texture.nativeTexture());

        if (data != nullptr && data->isValid())
            bindTexture(webgpuSampledView(*data),
                        slot,
                        sampler,
                        filtering,
                        context()->isFilterable(data->format));
    }

    void bindDepthTexture(const Texture& renderTarget,
                          int slot,
                          const wgpu::Sampler& sampler,
                          bool filtering,
                          int slice = -1)
    {
        auto* data = static_cast<WebGPUTextureData*>(renderTarget.nativeTexture());

        if (data != nullptr && data->depth != nullptr && data->sampleableDepth)
            bindTexture(webgpuDepthView(*data, slice), slot, sampler, filtering);
    }

    void bindSampler(int slot, const Sampler& sampler)
    {
        if (isTextureSlot(slot) && sampler.isValid())
            bindings.bindSampler(webgpuSamplerBinding(slot),
                                 nativeOf(sampler),
                                 isFiltering(sampler.descriptor()));
    }

    void bindStorageBuffer(const BufferRange& range, int slot)
    {
        if (encoder == nullptr || slot < 0 || slot >= ComputePass::maxBufferSlots
            || range.buffer == nullptr)
            return;

        auto* data = bufferDataOf(*range.buffer);

        if (data == nullptr || range.offset < 0
            || static_cast<std::uint64_t>(range.offset) >= data->size)
            return;

        const auto offset = static_cast<std::uint64_t>(range.offset);

        if (!context()->isStorageOffset(offset))
            return;

        const auto available = data->size - offset;
        const auto wanted =
            range.bytes > 0 ? static_cast<std::uint64_t>(range.bytes) : available;

        bindings.bindBuffer(webgpuBufferBinding(slot),
                            data->buffer,
                            offset,
                            alignTo(std::min(wanted, available), 4));
    }

    void uploadUniforms(const void* data, int bytes, int slot)
    {
        if (encoder == nullptr || bytes <= 0 || slot != 0 || context() == nullptr)
            return;

        auto uniforms = context()->uploadUniforms(
            *encoder->recording, data, static_cast<std::size_t>(bytes));

        if (uniforms.isValid())
            bindings.bindBuffer(webgpuUniformBinding,
                                uniforms.buffer,
                                uniforms.offset,
                                uniforms.size);
    }

    // The pipeline's variant whose layout declares unfilterable-float each
    // float texture binding that holds a texture WebGPU will not filter, set
    // on the pass when it differs from the one set.
    bool selectVariantForBoundTextures()
    {
        const auto unfilterable =
            bindings.unfilterableBindings(pipeline->layout.bindings);
        const auto* layout = &pipeline->layoutFor(unfilterable);

        if (layout == activeLayout)
            return true;

        auto native = pipeline->pipelineFor(encoder->depthFormat, unfilterable);

        if (native == nullptr)
            return false;

        activeLayout = layout;
        group = nullptr;
        encoder->pass.SetPipeline(native);
        return true;
    }

    bool prepareDraw()
    {
        if (!canRecord())
            return false;

        if (bindings.isDirty() || group == nullptr)
        {
            if (!selectVariantForBoundTextures())
                return false;

            group = bindings.makeGroup(*context(), *activeLayout);

            if (group == nullptr && activeLayout->group != nullptr)
                return false;

            if (group != nullptr)
                encoder->pass.SetBindGroup(0, group, 0, nullptr);
        }

        return true;
    }

    bool bindIndexRange(const BufferRange& indices, IndexFormat format)
    {
        if (indices.buffer == nullptr)
            return false;

        auto* data = bufferDataOf(*indices.buffer);

        if (data == nullptr || indices.offset < 0
            || static_cast<std::uint64_t>(indices.offset) >= data->size)
            return false;

        const auto offset = static_cast<std::uint64_t>(indices.offset);
        const auto size = indices.bytes > 0
                              ? static_cast<std::uint64_t>(indices.bytes)
                              : data->size - offset;

        encoder->pass.SetIndexBuffer(data->buffer,
                                     format == IndexFormat::UInt16
                                         ? wgpu::IndexFormat::Uint16
                                         : wgpu::IndexFormat::Uint32,
                                     offset,
                                     std::min(size, data->size - offset));
        return true;
    }

    // WebGPU has no indirect draw that takes a count, so a run of records is
    // one call per record.
    template <typename Record>
    void recordIndirectRuns(const IndirectDrawPlan& plan, Record&& record)
    {
        for (auto draw = 0; draw < plan.drawCount; ++draw)
            record(static_cast<std::uint64_t>(plan.offset + draw * plan.stride));
    }

    OwningPointer<WebGPURenderEncoder> encoder;
    int targetWidth = 0;
    int targetHeight = 0;
    WebGPURenderPipeline* pipeline = nullptr;
    const WebGPUBindingLayout* activeLayout = nullptr;
    WebGPUBindingTable bindings;
    wgpu::BindGroup group;
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

    const auto left =
        std::clamp(static_cast<int>(std::floor(rect.x)), 0, impl->targetWidth);
    const auto top =
        std::clamp(static_cast<int>(std::floor(rect.y)), 0, impl->targetHeight);
    const auto right = std::clamp(
        static_cast<int>(std::ceil(rect.x + rect.w)), left, impl->targetWidth);
    const auto bottom = std::clamp(
        static_cast<int>(std::ceil(rect.y + rect.h)), top, impl->targetHeight);

    impl->encoder->pass.SetScissorRect(static_cast<std::uint32_t>(left),
                                       static_cast<std::uint32_t>(top),
                                       static_cast<std::uint32_t>(right - left),
                                       static_cast<std::uint32_t>(bottom - top));
}

void RenderPass::clearScissorRect()
{
    if (!impl->encoder || impl->targetWidth <= 0 || impl->targetHeight <= 0)
        return;

    impl->encoder->pass.SetScissorRect(
        0,
        0,
        static_cast<std::uint32_t>(impl->targetWidth),
        static_cast<std::uint32_t>(impl->targetHeight));
}

// Framebuffer coordinates are y down in WebGPU as in Metal, so the viewport is
// the rect as given.
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

    impl->encoder->pass.SetViewport(
        rect.x, rect.y, rect.w, rect.h, nearDepth, farDepth);
}

void RenderPass::clearViewport()
{
    if (!impl->encoder || impl->targetWidth <= 0 || impl->targetHeight <= 0)
        return;

    impl->encoder->pass.SetViewport(0.f,
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

    auto* state = static_cast<WebGPURenderPipeline*>(pipeline.nativeState());
    impl->pipeline = nullptr;

    if (state == nullptr || !state->valid)
        return;

    auto native = state->pipelineFor(impl->encoder->depthFormat);

    if (native == nullptr)
        return;

    impl->pipeline = state;
    impl->activeLayout = &state->layout;
    impl->group = nullptr;
    impl->encoder->pass.SetPipeline(native);
}

void RenderPass::setStencilReference(unsigned int value)
{
    if (impl->encoder)
        impl->encoder->pass.SetStencilReference(static_cast<std::uint32_t>(value));
}

void RenderPass::setVertexBuffer(const Buffer& buffer, int index)
{
    setVertexBuffer(BufferRange::of(buffer), index);
}

void RenderPass::setVertexBuffer(const BufferRange& range, int index)
{
    if (!impl->encoder || range.buffer == nullptr || index < 0)
        return;

    auto* data = bufferDataOf(*range.buffer);

    if (data == nullptr || range.offset < 0
        || static_cast<std::uint64_t>(range.offset) >= data->size)
        return;

    const auto offset = static_cast<std::uint64_t>(range.offset);
    impl->encoder->pass.SetVertexBuffer(static_cast<std::uint32_t>(index),
                                        data->buffer,
                                        offset,
                                        data->size - offset);
}

void RenderPass::setFragmentTexture(const Texture& texture,
                                    int slot,
                                    TextureSampling sampling)
{
    if (auto* context = impl->context())
        impl->bindColorTexture(
            texture, slot, context->getSampler(sampling), isFiltering(sampling));
}

void RenderPass::setFragmentTexture(const Texture& texture,
                                    int slot,
                                    const Sampler& sampler)
{
    if (sampler.isValid())
        impl->bindColorTexture(
            texture, slot, nativeOf(sampler), isFiltering(sampler.descriptor()));
}

void RenderPass::setFragmentDepthTexture(const Texture& renderTarget,
                                         int slot,
                                         TextureSampling sampling)
{
    if (auto* context = impl->context())
        impl->bindDepthTexture(renderTarget,
                               slot,
                               context->getSampler(sampling),
                               isFiltering(sampling));
}

void RenderPass::setFragmentDepthTexture(const Texture& renderTarget,
                                         int slot,
                                         const Sampler& sampler)
{
    if (sampler.isValid())
        impl->bindDepthTexture(renderTarget,
                               slot,
                               nativeOf(sampler),
                               isFiltering(sampler.descriptor()));
}

void RenderPass::setFragmentDepthTextureSlice(const Texture& renderTarget,
                                              int slice,
                                              int slot,
                                              TextureSampling sampling)
{
    if (auto* context = impl->context())
        impl->bindDepthTexture(renderTarget,
                               slot,
                               context->getSampler(sampling),
                               isFiltering(sampling),
                               slice);
}

void RenderPass::setFragmentDepthTextureSlice(const Texture& renderTarget,
                                              int slice,
                                              int slot,
                                              const Sampler& sampler)
{
    if (sampler.isValid())
        impl->bindDepthTexture(renderTarget,
                               slot,
                               nativeOf(sampler),
                               isFiltering(sampler.descriptor()),
                               slice);
}

// One bind group serves both stages, so a vertex bind lands on the binding a
// fragment one would.
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

void RenderPass::setFragmentSampler(const Sampler& sampler, int slot)
{
    impl->bindSampler(slot, sampler);
}

void RenderPass::setVertexSampler(const Sampler& sampler, int slot)
{
    impl->bindSampler(slot, sampler);
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
    if (vertexCount <= 0 || instanceCount <= 0 || !impl->prepareDraw())
        return;

    impl->encoder->pass.Draw(static_cast<std::uint32_t>(vertexCount),
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
    if (indexCount <= 0 || instanceCount <= 0 || !impl->canRecord()
        || !impl->bindIndexRange(indices, format) || !impl->prepareDraw())
        return;

    impl->encoder->pass.DrawIndexed(static_cast<std::uint32_t>(indexCount),
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
    auto* data = bufferDataOf(arguments);

    if (!plan || data == nullptr || !impl->prepareDraw())
        return;

    impl->recordIndirectRuns(
        *plan,
        [&](std::uint64_t at)
        { impl->encoder->pass.DrawIndirect(data->buffer, at); });
}

void RenderPass::drawIndirectCount(
    const Buffer&, std::int64_t, const Buffer&, std::int64_t, int, std::int64_t)
{
    traceIndirectCountUnsupported();
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
    auto* data = bufferDataOf(arguments);

    if (!plan || data == nullptr || !impl->canRecord()
        || !impl->bindIndexRange(indices, format) || !impl->prepareDraw())
        return;

    impl->recordIndirectRuns(
        *plan,
        [&](std::uint64_t at)
        { impl->encoder->pass.DrawIndexedIndirect(data->buffer, at); });
}

void RenderPass::drawIndexedIndirectCount(const BufferRange&,
                                          IndexFormat,
                                          const Buffer&,
                                          std::int64_t,
                                          const Buffer&,
                                          std::int64_t,
                                          int,
                                          std::int64_t)
{
    traceIndirectCountUnsupported();
}

// Device::supportsOcclusionQueries is false here, so no set reaches a pass.
void RenderPass::nativeBeginOcclusionQuery(int, OcclusionMode) {}

void RenderPass::nativeEndOcclusionQuery(int, OcclusionMode) {}

void RenderPass::resolveOcclusionQueries() {}

void RenderPass::end()
{
    drainParticipants();

    if (!impl->encoder)
        return;

    endOcclusionQuery();
    impl->encoder->pass.End();

    if (impl->encoder->recording != nullptr)
        impl->encoder->recording->passOpen = false;

    impl->encoder.reset();
    impl->pipeline = nullptr;
}
} // namespace eacp::GPU

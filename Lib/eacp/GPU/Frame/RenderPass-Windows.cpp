#include <eacp/Core/Utils/WinInclude.h>

#include "RenderPass.h"

#include "IndirectDrawPlan.h"
#include "RenderPassBindings-Windows.h"
#include "RenderPassRegions.h"

#include "../Buffer/Buffer.h"
#include "../Pipeline/RenderPipeline.h"
#include "../Windows/D3D12Types.h"

// Windows/D3D12 backend. Records draw commands onto the frame's recording via
// the D3D12Encoder. The encoder is owned here so it is freed when the pass
// goes out of scope; the CommandContext it points at stays owned by the Frame,
// which submits and presents on destruction.

namespace eacp::GPU
{
struct RenderPass::Native
{
    Native(void* encoderHandle, int width, int height)
        : encoder(static_cast<D3D12Encoder*>(encoderHandle))
        , targetWidth(width)
        , targetHeight(height)
    {
    }

    std::unique_ptr<D3D12Encoder> encoder;

    // Render target size in pixels, for clamping scissor rects.
    int targetWidth = 0;
    int targetHeight = 0;

    // Whether a valid pipeline state is currently bound. A pipeline whose
    // compilation failed has a null state; drawing without one is flagged by the
    // D3D12 debug layer, so draws are skipped when false.
    bool pipelineBound = false;

    bool canSetTargetState() const
    {
        return encoder && hasTargetArea(targetWidth, targetHeight);
    }

    bool canDraw() const { return encoder && pipelineBound; }

    bool hasCommands() const { return encoder && encoder->commands != nullptr; }

    ID3D12GraphicsCommandList* list() const { return encoder->commands->list.get(); }

    void recordIndexedIndirectDraws(const IndexRange& indexRange,
                                    const Buffer& arguments,
                                    const IndirectDrawPlan& plan,
                                    IndirectDrawCount count)
    {
        if (!canDraw())
            return;

        auto& commands = *encoder->commands;

        if (bindIndexRange(commands, indexRange.indices, indexRange.format))
            executeIndexedIndirectDraws(commands, plan, arguments, count);
    }
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
    if (!impl->canSetTargetState())
        return;

    const auto pixels =
        calculateScissorPixels(rect, impl->targetWidth, impl->targetHeight);

    const D3D12_RECT scissor {static_cast<LONG>(pixels.left),
                              static_cast<LONG>(pixels.top),
                              static_cast<LONG>(pixels.right),
                              static_cast<LONG>(pixels.bottom)};

    impl->list()->RSSetScissorRects(1, &scissor);
}

void RenderPass::clearScissorRect()
{
    if (!impl->canSetTargetState())
        return;

    const D3D12_RECT scissor {0,
                              0,
                              static_cast<LONG>(impl->targetWidth),
                              static_cast<LONG>(impl->targetHeight)};

    impl->list()->RSSetScissorRects(1, &scissor);
}

void RenderPass::setViewport(const Graphics::Rect& rect,
                             float nearDepth,
                             float farDepth)
{
    if (!impl->canSetTargetState())
        return;

    if (!isViewportInsideTarget(rect, impl->targetWidth, impl->targetHeight))
        return;

    D3D12_VIEWPORT viewport = {};
    viewport.TopLeftX = rect.x;
    viewport.TopLeftY = rect.y;
    viewport.Width = rect.w;
    viewport.Height = rect.h;
    viewport.MinDepth = nearDepth;
    viewport.MaxDepth = farDepth;

    impl->list()->RSSetViewports(1, &viewport);
}

void RenderPass::clearViewport()
{
    if (!impl->canSetTargetState())
        return;

    D3D12_VIEWPORT viewport = {};
    viewport.Width = static_cast<float>(impl->targetWidth);
    viewport.Height = static_cast<float>(impl->targetHeight);
    viewport.MaxDepth = 1.0f;

    impl->list()->RSSetViewports(1, &viewport);
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

    auto* state = static_cast<D3D12Pipeline*>(pipeline.nativeState());

    impl->pipelineBound = state != nullptr && state->state != nullptr;

    if (!impl->pipelineBound)
        return;

    auto* list = impl->list();
    list->SetPipelineState(state->state.get());
    list->IASetPrimitiveTopology(state->topology);

    impl->encoder->strides = state->strides;
}

void RenderPass::setStencilReference(unsigned int value)
{
    if (!impl->encoder)
        return;

    impl->list()->OMSetStencilRef(static_cast<UINT>(value));
}

void RenderPass::setVertexBuffer(const Buffer& buffer, int index)
{
    setVertexBuffer(BufferRange::of(buffer), index);
}

void RenderPass::setVertexBuffer(const BufferRange& range, int index)
{
    if (impl->encoder)
        bindVertexBufferRange(*impl->encoder, range, index);
}

void RenderPass::setFragmentTexture(const Texture& texture,
                                    int slot,
                                    TextureSampling)
{
    bindStageTexture(impl->encoder.get(), {ShaderStage::Fragment, slot}, texture);
}

void RenderPass::setFragmentTexture(const Texture& texture,
                                    int slot,
                                    const Sampler& sampler)
{
    setFragmentTexture(texture, slot, TextureSampling {});
    bindPerBindSampler(impl->encoder.get(), slot, sampler);
}

void RenderPass::setFragmentDepthTexture(const Texture& renderTarget,
                                         int slot,
                                         TextureSampling)
{
    bindStageDepthTexture(
        impl->encoder.get(), {ShaderStage::Fragment, slot}, renderTarget);
}

void RenderPass::setFragmentDepthTexture(const Texture& renderTarget,
                                         int slot,
                                         const Sampler& sampler)
{
    setFragmentDepthTexture(renderTarget, slot, TextureSampling {});
    bindPerBindSampler(impl->encoder.get(), slot, sampler);
}

void RenderPass::setFragmentDepthTextureSlice(const Texture& renderTarget,
                                              int slice,
                                              int slot,
                                              TextureSampling)
{
    bindStageDepthSlice(
        impl->encoder.get(), {ShaderStage::Fragment, slot}, renderTarget, slice);
}

void RenderPass::setFragmentDepthTextureSlice(const Texture& renderTarget,
                                              int slice,
                                              int slot,
                                              const Sampler& sampler)
{
    setFragmentDepthTextureSlice(renderTarget, slice, slot, TextureSampling {});
    bindPerBindSampler(impl->encoder.get(), slot, sampler);
}

void RenderPass::setVertexDepthTextureSlice(const Texture& renderTarget,
                                            int slice,
                                            int slot,
                                            TextureSampling)
{
    bindStageDepthSlice(
        impl->encoder.get(), {ShaderStage::Vertex, slot}, renderTarget, slice);
}

void RenderPass::setVertexDepthTextureSlice(const Texture& renderTarget,
                                            int slice,
                                            int slot,
                                            const Sampler& sampler)
{
    setVertexDepthTextureSlice(renderTarget, slice, slot, TextureSampling {});
    bindPerBindSampler(impl->encoder.get(), slot, sampler);
}

// The tables are visible to every stage (see createRootSignatures), so these
// bind the same root parameters the fragment calls do.
void RenderPass::setVertexTexture(const Texture& texture, int slot, TextureSampling)
{
    bindStageTexture(impl->encoder.get(), {ShaderStage::Vertex, slot}, texture);
}

void RenderPass::setVertexTexture(const Texture& texture,
                                  int slot,
                                  const Sampler& sampler)
{
    setVertexTexture(texture, slot, TextureSampling {});
    bindPerBindSampler(impl->encoder.get(), slot, sampler);
}

void RenderPass::setVertexDepthTexture(const Texture& renderTarget,
                                       int slot,
                                       TextureSampling)
{
    bindStageDepthTexture(
        impl->encoder.get(), {ShaderStage::Vertex, slot}, renderTarget);
}

void RenderPass::setVertexDepthTexture(const Texture& renderTarget,
                                       int slot,
                                       const Sampler& sampler)
{
    setVertexDepthTexture(renderTarget, slot, TextureSampling {});
    bindPerBindSampler(impl->encoder.get(), slot, sampler);
}

static bool isRenderPassSlotInRange(int slot, int slotCount)
{
    return slot >= 0 && slot < slotCount;
}

void RenderPass::setVertexStorageBuffer(const Buffer& buffer, int slot)
{
    setVertexStorageBuffer(BufferRange::of(buffer), slot);
}

void RenderPass::setVertexStorageBuffer(const BufferRange& range, int slot)
{
    if (!impl->encoder || !isRenderPassSlotInRange(slot, maxBufferSlots))
        return;

    auto& commands = *impl->encoder->commands;

    if (auto address = storageBufferAddress(commands, range))
        commands.list->SetGraphicsRootShaderResourceView(renderVertexSRVParam(slot),
                                                         address);
}

void RenderPass::setFragmentStorageBuffer(const Buffer& buffer, int slot)
{
    setFragmentStorageBuffer(BufferRange::of(buffer), slot);
}

void RenderPass::setFragmentStorageBuffer(const BufferRange& range, int slot)
{
    if (!impl->encoder || !isRenderPassSlotInRange(slot, maxBufferSlots))
        return;

    auto& commands = *impl->encoder->commands;

    if (auto address = storageBufferAddress(commands, range))
        commands.list->SetGraphicsRootShaderResourceView(renderPixelSRVParam(slot),
                                                         address);
}

void RenderPass::setVertexBytes(const void* data, int bytes, int slot)
{
    if (impl->encoder && isRenderPassSlotInRange(slot, maxUniformSlots))
        bindConstantBytes(*impl->encoder, renderVertexCBVParam(slot), data, bytes);
}

void RenderPass::setFragmentBytes(const void* data, int bytes, int slot)
{
    if (impl->encoder && isRenderPassSlotInRange(slot, maxUniformSlots))
        bindConstantBytes(*impl->encoder, renderPixelCBVParam(slot), data, bytes);
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
    if (!impl->canDraw())
        return;

    impl->list()->DrawInstanced(static_cast<UINT>(vertexCount),
                                static_cast<UINT>(instanceCount),
                                static_cast<UINT>(firstVertex),
                                static_cast<UINT>(firstInstance));
}

void RenderPass::drawIndirect(const Buffer& arguments,
                              std::int64_t offset,
                              int drawCount,
                              std::int64_t stride)
{
    const auto plan = planIndirectDraws(
        arguments, offset, drawCount, stride, sizeof(DrawArguments));

    if (plan && impl->canDraw())
        executeIndirectDraws(*impl->encoder->commands, *plan, arguments, {});
}

void RenderPass::drawIndirectCount(const Buffer& arguments,
                                   std::int64_t offset,
                                   const Buffer& count,
                                   std::int64_t countOffset,
                                   int maxDrawCount,
                                   std::int64_t stride)
{
    const auto plan = planIndirectDraws(
        arguments, offset, maxDrawCount, stride, sizeof(DrawArguments));

    if (!plan || !isValidDrawCountBuffer(count, countOffset))
        return;

    if (impl->canDraw())
        executeIndirectDraws(
            *impl->encoder->commands, *plan, arguments, {&count, countOffset});
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

    if (plan)
        impl->recordIndexedIndirectDraws({indices, format}, arguments, *plan, {});
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
    const auto plan = planIndirectDraws(
        arguments, offset, maxDrawCount, stride, sizeof(DrawIndexedArguments));

    if (plan && isValidDrawCountBuffer(count, countOffset))
        impl->recordIndexedIndirectDraws(
            {indices, format}, arguments, *plan, {&count, countOffset});
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
    if (!impl->canDraw())
        return;

    auto& commands = *impl->encoder->commands;

    if (!bindIndexRange(commands, indices, format))
        return;

    commands.list->DrawIndexedInstanced(static_cast<UINT>(indexCount),
                                        static_cast<UINT>(instanceCount),
                                        static_cast<UINT>(firstIndex),
                                        static_cast<INT>(baseVertex),
                                        static_cast<UINT>(firstInstance));
}

// OCCLUSION counts samples and BINARY_OCCLUSION says whether any passed, both
// out of the one OCCLUSION heap the set owns.
static D3D12_QUERY_TYPE d3d12OcclusionType(OcclusionMode mode)
{
    return mode == OcclusionMode::AnySamples ? D3D12_QUERY_TYPE_BINARY_OCCLUSION
                                             : D3D12_QUERY_TYPE_OCCLUSION;
}

void RenderPass::nativeBeginOcclusionQuery(int index, OcclusionMode mode)
{
    auto* heap = static_cast<ID3D12QueryHeap*>(occlusionQueries->nativeQueries());

    if (impl->hasCommands() && heap != nullptr)
        impl->list()->BeginQuery(
            heap, d3d12OcclusionType(mode), static_cast<UINT>(index));
}

void RenderPass::nativeEndOcclusionQuery(int index, OcclusionMode mode)
{
    auto* heap = static_cast<ID3D12QueryHeap*>(occlusionQueries->nativeQueries());

    if (impl->hasCommands() && heap != nullptr)
        impl->list()->EndQuery(
            heap, d3d12OcclusionType(mode), static_cast<UINT>(index));
}

// Each query the pass used, resolved into its eight bytes of the set's
// readback buffer, which rests in COPY_DEST as a readback heap's resources
// must. Only those: a query this pass did not begin keeps what an earlier
// frame resolved for it.
void RenderPass::resolveOcclusionQueries()
{
    if (occlusionQueries == nullptr || !impl->hasCommands())
        return;

    auto* heap = static_cast<ID3D12QueryHeap*>(occlusionQueries->nativeQueries());
    auto* readback = static_cast<ID3D12Resource*>(occlusionQueries->nativeResults());

    if (heap == nullptr || readback == nullptr)
        return;

    for (const auto& used: usedQueries)
        impl->list()->ResolveQueryData(heap,
                                       d3d12OcclusionType(used.mode),
                                       static_cast<UINT>(used.index),
                                       1,
                                       readback,
                                       static_cast<UINT64>(used.index)
                                           * sizeof(std::uint64_t));
}

void RenderPass::end()
{
    // Before the encoder goes, so a batching renderer's queued draws still get
    // recorded. See RenderPass::Participant.
    drainParticipants();

    // After them, so a participant's draws inside an open query count, and the
    // resolve after that, every query of the pass now closed.
    endOcclusionQuery();
    resolveOcclusionQueries();

    // After them, so what a participant flushed is inside the pass being timed
    // rather than after it.
    if (impl->encoder)
        endTimedPass(*impl->encoder);

    // And after the timestamp, so the resolve is not counted as the pass's own
    // work: it belongs to the target rather than to what was drawn. Metal spells
    // this as a store action on the attachment and records nothing at all here -
    // see resolveMultisampledTarget for why it happens per pass on both.
    if (impl->hasCommands())
        for (auto* target: impl->encoder->resolveTargets)
            resolveMultisampledTarget(*impl->encoder->commands, *target);

    // Commands are recorded onto the frame's list, which submits when the
    // frame is destroyed; releasing the encoder marks the pass finished.
    impl->encoder.reset();
}
} // namespace eacp::GPU

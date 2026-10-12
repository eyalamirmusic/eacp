#include <eacp/Core/Utils/WinInclude.h>

#include "RenderPass.h"

#include "IndirectDrawPlan.h"

#include "../Buffer/Buffer.h"
#include "../Pipeline/RenderPipeline.h"
#include "../Shader/ShaderSource.h"
#include "../Texture/Sampler.h"
#include "../Texture/Texture.h"
#include "../Windows/D3D12Types.h"

#include <algorithm>
#include <cmath>

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

    const D3D12_RECT scissor {static_cast<LONG>(left),
                              static_cast<LONG>(top),
                              static_cast<LONG>(right),
                              static_cast<LONG>(bottom)};

    impl->encoder->commands->list->RSSetScissorRects(1, &scissor);
}

void RenderPass::clearScissorRect()
{
    if (!impl->encoder || impl->targetWidth <= 0 || impl->targetHeight <= 0)
        return;

    const D3D12_RECT scissor {0,
                              0,
                              static_cast<LONG>(impl->targetWidth),
                              static_cast<LONG>(impl->targetHeight)};

    impl->encoder->commands->list->RSSetScissorRects(1, &scissor);
}

void RenderPass::setViewport(const Graphics::Rect& rect,
                             float nearDepth,
                             float farDepth)
{
    if (!impl->encoder || impl->targetWidth <= 0 || impl->targetHeight <= 0)
        return;

    // No rounding, unlike the scissor: a viewport is a float rectangle in both
    // APIs, and rounding it would move the mapping rather than the clip.
    if (rect.w <= 0.f || rect.h <= 0.f || rect.x < 0.f || rect.y < 0.f
        || rect.x + rect.w > static_cast<float>(impl->targetWidth)
        || rect.y + rect.h > static_cast<float>(impl->targetHeight))
        return;

    D3D12_VIEWPORT viewport = {};
    viewport.TopLeftX = rect.x;
    viewport.TopLeftY = rect.y;
    viewport.Width = rect.w;
    viewport.Height = rect.h;
    viewport.MinDepth = nearDepth;
    viewport.MaxDepth = farDepth;

    impl->encoder->commands->list->RSSetViewports(1, &viewport);
}

void RenderPass::clearViewport()
{
    if (!impl->encoder || impl->targetWidth <= 0 || impl->targetHeight <= 0)
        return;

    D3D12_VIEWPORT viewport = {};
    viewport.Width = static_cast<float>(impl->targetWidth);
    viewport.Height = static_cast<float>(impl->targetHeight);
    viewport.MaxDepth = 1.0f;

    impl->encoder->commands->list->RSSetViewports(1, &viewport);
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

    auto* list = impl->encoder->commands->list.get();
    list->SetPipelineState(state->state.get());
    list->IASetPrimitiveTopology(state->topology);

    impl->encoder->strides = state->strides;
}

void RenderPass::setStencilReference(unsigned int value)
{
    if (!impl->encoder)
        return;

    impl->encoder->commands->list->OMSetStencilRef(static_cast<UINT>(value));
}

void RenderPass::setVertexBuffer(const Buffer& buffer, int index)
{
    setVertexBuffer(BufferRange::of(buffer), index);
}

void RenderPass::setVertexBuffer(const BufferRange& range, int index)
{
    if (!impl->encoder || range.buffer == nullptr || range.offset < 0)
        return;

    auto* data = static_cast<D3D12BufferData*>(range.buffer->nativeBuffer());

    if (data == nullptr || data->resource == nullptr
        || (UINT64) range.offset >= data->size)
        return;

    auto& commands = *impl->encoder->commands;
    transitionForUse(
        commands, *data, D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER);

    // A view starting part-way into the resource: the address moves up by the
    // offset and the size comes down by it, so the view still ends where the
    // buffer does and vertex zero is the byte at the offset.
    D3D12_VERTEX_BUFFER_VIEW view = {};
    view.BufferLocation =
        data->resource->GetGPUVirtualAddress() + (UINT64) range.offset;
    view.SizeInBytes = static_cast<UINT>(data->size - (UINT64) range.offset);
    view.StrideInBytes = strideForSlot(impl->encoder->strides, index);

    commands.list->IASetVertexBuffers(static_cast<UINT>(index), 1, &view);
}

namespace
{
const auto anyShaderRead = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE
                           | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;

// A vertex read needs NON_PIXEL_SHADER_RESOURCE, and asks for both read states
// so that a fragment bind of the same texture in the same draw finds its own
// already there rather than barriering the vertex one away.
bool hasReadState(D3D12_RESOURCE_STATES state, D3D12_RESOURCE_STATES wanted)
{
    return (state & wanted) == wanted;
}

D3D12_RESOURCE_STATES readStateFor(ShaderStage stage)
{
    return stage == ShaderStage::Vertex ? anyShaderRead
                                        : D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
}

void bindStageTexture(D3D12Encoder* encoder,
                      ShaderStage stage,
                      const Texture& texture,
                      int slot)
{
    if (encoder == nullptr || slot < 0 || slot >= maxTextureSlots)
        return;

    auto* data = static_cast<D3D12TextureData*>(texture.nativeTexture());

    if (data == nullptr || data->srv.gpu.ptr == 0)
        return;

    auto* list = encoder->commands->list.get();

    // A render target an earlier pass on this frame drew into is still in
    // RENDER_TARGET, and this is where it goes back. A plain texture is already
    // in the state this asks for and the helper records nothing.
    if (!hasReadState(data->state, readStateFor(stage)))
        transitionTextureForUse(list, *data, readStateFor(stage));

    // Only the SRV: the sampler is a static sampler in the root signature, chosen
    // by the register the shader's sampler was emitted at. See TextureSampling.
    list->SetGraphicsRootDescriptorTable(renderTextureParam(slot), data->srv.gpu);
}

void bindStageDepthTexture(D3D12Encoder* encoder,
                           ShaderStage stage,
                           const Texture& renderTarget,
                           int slot)
{
    if (encoder == nullptr || slot < 0 || slot >= maxTextureSlots)
        return;

    auto* data = static_cast<D3D12TextureData*>(renderTarget.nativeTexture());

    if (data == nullptr || !data->hasSampleableDepth()
        || data->depthSrv.gpu.ptr == 0)
        return;

    auto* list = encoder->commands->list.get();

    // The depth resource rests in DEPTH_WRITE, which is where the pass that drew
    // it left it and where the next pass to attach it expects to find it - so
    // this is the outward half of the pair, and Frame::beginPass records the
    // return. The two states are two views of one resource, which is why they
    // are one tracked value rather than a flag per view.
    if (!hasReadState(data->depthState, readStateFor(stage)))
        transitionDepthForUse(list, *data, readStateFor(stage));

    list->SetGraphicsRootDescriptorTable(renderTextureParam(slot),
                                         data->depthSrv.gpu);
}

// The same transition, through one slice's view rather than the whole plane's.
void bindStageDepthSlice(D3D12Encoder* encoder,
                         ShaderStage stage,
                         const Texture& renderTarget,
                         int slice,
                         int slot)
{
    if (encoder == nullptr || slot < 0 || slot >= maxTextureSlots)
        return;

    auto* data = static_cast<D3D12TextureData*>(renderTarget.nativeTexture());

    if (data == nullptr || !data->hasSampleableDepth())
        return;

    const auto view = data->depthSliceView(slice);

    if (view.ptr == 0)
        return;

    auto* list = encoder->commands->list.get();

    if (!hasReadState(data->depthState, readStateFor(stage)))
        transitionDepthForUse(list, *data, readStateFor(stage));

    list->SetGraphicsRootDescriptorTable(renderTextureParam(slot), view);
}

void bindPerBindSampler(D3D12Encoder* encoder, int slot, const Sampler& sampler)
{
    if (encoder == nullptr || slot < 0 || slot >= maxTextureSlots)
        return;

    const auto descriptor = perBindSamplerDescriptor(sampler);

    if (descriptor.ptr == 0)
        return;

    encoder->commands->list->SetGraphicsRootDescriptorTable(renderSamplerParam(slot),
                                                            descriptor);
}
} // namespace

void RenderPass::setFragmentTexture(const Texture& texture,
                                    int slot,
                                    TextureSampling)
{
    bindStageTexture(impl->encoder.get(), ShaderStage::Fragment, texture, slot);
}

void RenderPass::setFragmentTexture(const Texture& texture,
                                    int slot,
                                    const Sampler& sampler)
{
    bindStageTexture(impl->encoder.get(), ShaderStage::Fragment, texture, slot);
    bindPerBindSampler(impl->encoder.get(), slot, sampler);
}

void RenderPass::setFragmentDepthTexture(const Texture& renderTarget,
                                         int slot,
                                         TextureSampling)
{
    bindStageDepthTexture(
        impl->encoder.get(), ShaderStage::Fragment, renderTarget, slot);
}

void RenderPass::setFragmentDepthTexture(const Texture& renderTarget,
                                         int slot,
                                         const Sampler& sampler)
{
    bindStageDepthTexture(
        impl->encoder.get(), ShaderStage::Fragment, renderTarget, slot);
    bindPerBindSampler(impl->encoder.get(), slot, sampler);
}

void RenderPass::setFragmentDepthTextureSlice(const Texture& renderTarget,
                                              int slice,
                                              int slot,
                                              TextureSampling)
{
    bindStageDepthSlice(
        impl->encoder.get(), ShaderStage::Fragment, renderTarget, slice, slot);
}

void RenderPass::setFragmentDepthTextureSlice(const Texture& renderTarget,
                                              int slice,
                                              int slot,
                                              const Sampler& sampler)
{
    bindStageDepthSlice(
        impl->encoder.get(), ShaderStage::Fragment, renderTarget, slice, slot);
    bindPerBindSampler(impl->encoder.get(), slot, sampler);
}

void RenderPass::setVertexDepthTextureSlice(const Texture& renderTarget,
                                            int slice,
                                            int slot,
                                            TextureSampling)
{
    bindStageDepthSlice(
        impl->encoder.get(), ShaderStage::Vertex, renderTarget, slice, slot);
}

void RenderPass::setVertexDepthTextureSlice(const Texture& renderTarget,
                                            int slice,
                                            int slot,
                                            const Sampler& sampler)
{
    bindStageDepthSlice(
        impl->encoder.get(), ShaderStage::Vertex, renderTarget, slice, slot);
    bindPerBindSampler(impl->encoder.get(), slot, sampler);
}

// The tables are visible to every stage (see createRootSignatures), so these
// bind the same root parameters the fragment calls do.
void RenderPass::setVertexTexture(const Texture& texture, int slot, TextureSampling)
{
    bindStageTexture(impl->encoder.get(), ShaderStage::Vertex, texture, slot);
}

void RenderPass::setVertexTexture(const Texture& texture,
                                  int slot,
                                  const Sampler& sampler)
{
    bindStageTexture(impl->encoder.get(), ShaderStage::Vertex, texture, slot);
    bindPerBindSampler(impl->encoder.get(), slot, sampler);
}

void RenderPass::setVertexDepthTexture(const Texture& renderTarget,
                                       int slot,
                                       TextureSampling)
{
    bindStageDepthTexture(
        impl->encoder.get(), ShaderStage::Vertex, renderTarget, slot);
}

void RenderPass::setVertexDepthTexture(const Texture& renderTarget,
                                       int slot,
                                       const Sampler& sampler)
{
    bindStageDepthTexture(
        impl->encoder.get(), ShaderStage::Vertex, renderTarget, slot);
    bindPerBindSampler(impl->encoder.get(), slot, sampler);
}

namespace
{
// The address a stage's root SRV binds to, with the buffer moved into the state
// a shader read needs. A kernel that wrote this buffer left it in
// UNORDERED_ACCESS; the barrier here is what orders the draw behind that write.
// Zero when there is nothing to bind - no buffer, no resource, or an offset
// outside it - and the shader's element zero is the element at the offset.
D3D12_GPU_VIRTUAL_ADDRESS storageBufferAddress(CommandContext& commands,
                                               const BufferRange& range)
{
    if (range.buffer == nullptr || range.offset < 0)
        return 0;

    auto* data = static_cast<D3D12BufferData*>(range.buffer->nativeBuffer());

    if (data == nullptr || data->resource == nullptr
        || (UINT64) range.offset >= data->size)
        return 0;

    // The union of the two read states rather than one: the same buffer may be
    // bound to both stages, and asking for each in turn would barrier it back
    // and forth between two states that are both just "a shader is reading it".
    transitionForUse(commands,
                     *data,
                     D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE
                         | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);

    return data->resource->GetGPUVirtualAddress() + (UINT64) range.offset;
}
} // namespace

void RenderPass::setVertexStorageBuffer(const Buffer& buffer, int slot)
{
    setVertexStorageBuffer(BufferRange::of(buffer), slot);
}

void RenderPass::setVertexStorageBuffer(const BufferRange& range, int slot)
{
    if (!impl->encoder || slot < 0 || slot >= maxBufferSlots)
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
    if (!impl->encoder || slot < 0 || slot >= maxBufferSlots)
        return;

    auto& commands = *impl->encoder->commands;

    if (auto address = storageBufferAddress(commands, range))
        commands.list->SetGraphicsRootShaderResourceView(renderPixelSRVParam(slot),
                                                         address);
}

void RenderPass::setVertexBytes(const void* data, int bytes, int slot)
{
    if (!impl->encoder || slot < 0 || slot >= maxUniformSlots)
        return;

    auto& commands = *impl->encoder->commands;
    auto address =
        commands.context->uploadConstants(commands, data, (std::size_t) bytes);

    if (address != 0)
        commands.list->SetGraphicsRootConstantBufferView(renderVertexCBVParam(slot),
                                                         address);
}

void RenderPass::setFragmentBytes(const void* data, int bytes, int slot)
{
    if (!impl->encoder || slot < 0 || slot >= maxUniformSlots)
        return;

    auto& commands = *impl->encoder->commands;
    auto address =
        commands.context->uploadConstants(commands, data, (std::size_t) bytes);

    if (address != 0)
        commands.list->SetGraphicsRootConstantBufferView(renderPixelCBVParam(slot),
                                                         address);
}

void RenderPass::draw(int vertexCount, int firstVertex)
{
    if (!impl->encoder || !impl->pipelineBound)
        return;

    impl->encoder->commands->list->DrawInstanced(
        static_cast<UINT>(vertexCount), 1, static_cast<UINT>(firstVertex), 0);
}

void RenderPass::drawInstanced(int vertexCount,
                               int instanceCount,
                               int firstVertex,
                               int firstInstance)
{
    if (!impl->encoder || !impl->pipelineBound)
        return;

    impl->encoder->commands->list->DrawInstanced(static_cast<UINT>(vertexCount),
                                                 static_cast<UINT>(instanceCount),
                                                 static_cast<UINT>(firstVertex),
                                                 static_cast<UINT>(firstInstance));
}

namespace
{
// The index buffer view for a slice, bound, with the resource moved into the
// state an index read needs. False when there is nothing to bind - no buffer,
// no resource, or an offset past the end of it - so the draw that follows can
// be skipped rather than issued over nothing. The view starts at the slice
// and runs to the buffer's end, the same shape the vertex bind takes, so
// firstIndex counts on from the slice's own index zero.
bool bindIndexRange(CommandContext& commands,
                    const BufferRange& indices,
                    IndexFormat format)
{
    if (indices.buffer == nullptr || indices.offset < 0)
        return false;

    auto* data = static_cast<D3D12BufferData*>(indices.buffer->nativeBuffer());

    if (data == nullptr || data->resource == nullptr
        || (UINT64) indices.offset >= data->size)
        return false;

    transitionForUse(commands, *data, D3D12_RESOURCE_STATE_INDEX_BUFFER);

    D3D12_INDEX_BUFFER_VIEW view = {};
    view.BufferLocation =
        data->resource->GetGPUVirtualAddress() + (UINT64) indices.offset;
    view.SizeInBytes = static_cast<UINT>(data->size - (UINT64) indices.offset);
    view.Format =
        format == IndexFormat::UInt16 ? DXGI_FORMAT_R16_UINT : DXGI_FORMAT_R32_UINT;

    commands.list->IASetIndexBuffer(&view);

    return true;
}
} // namespace

namespace
{
D3D12BufferData* indirectArgumentsOf(CommandContext& commands, const Buffer& buffer)
{
    auto* data = static_cast<D3D12BufferData*>(buffer.nativeBuffer());

    if (data == nullptr || data->resource == nullptr)
        return nullptr;

    transitionForUse(commands, *data, D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT);
    return data;
}

// One ExecuteIndirect for the whole run: the stride is the signature's, and
// the count buffer, where there is one, caps the run at the GPU's own number.
void executeIndirectDraws(CommandContext& commands,
                          bool indexed,
                          const IndirectDrawPlan& plan,
                          const Buffer& arguments,
                          const Buffer* count,
                          std::int64_t countOffset)
{
    auto* signature =
        commands.context->getDrawSignature(indexed, static_cast<UINT>(plan.stride));
    auto* data = indirectArgumentsOf(commands, arguments);
    auto* countData =
        count != nullptr ? indirectArgumentsOf(commands, *count) : nullptr;

    if (signature == nullptr || data == nullptr
        || (count != nullptr && countData == nullptr))
        return;

    commands.list->ExecuteIndirect(signature,
                                   static_cast<UINT>(plan.drawCount),
                                   data->resource.get(),
                                   static_cast<UINT64>(plan.offset),
                                   countData != nullptr ? countData->resource.get()
                                                        : nullptr,
                                   static_cast<UINT64>(countOffset));
}
} // namespace

void RenderPass::drawIndirect(const Buffer& arguments,
                              std::int64_t offset,
                              int drawCount,
                              std::int64_t stride)
{
    const auto plan = planIndirectDraws(
        arguments, offset, drawCount, stride, sizeof(DrawArguments));

    if (!plan || !impl->encoder || !impl->pipelineBound)
        return;

    executeIndirectDraws(
        *impl->encoder->commands, false, *plan, arguments, nullptr, 0);
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

    if (!plan || !isValidDrawCountBuffer(count, countOffset) || !impl->encoder
        || !impl->pipelineBound)
        return;

    executeIndirectDraws(
        *impl->encoder->commands, false, *plan, arguments, &count, countOffset);
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

    if (!plan || !impl->encoder || !impl->pipelineBound)
        return;

    auto& commands = *impl->encoder->commands;

    if (!bindIndexRange(commands, indices, format))
        return;

    executeIndirectDraws(commands, true, *plan, arguments, nullptr, 0);
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

    if (!plan || !isValidDrawCountBuffer(count, countOffset) || !impl->encoder
        || !impl->pipelineBound)
        return;

    auto& commands = *impl->encoder->commands;

    if (!bindIndexRange(commands, indices, format))
        return;

    executeIndirectDraws(commands, true, *plan, arguments, &count, countOffset);
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
    if (!impl->encoder || !impl->pipelineBound)
        return;

    auto& commands = *impl->encoder->commands;

    if (!bindIndexRange(commands, indices, format))
        return;

    commands.list->DrawIndexedInstanced(static_cast<UINT>(indexCount),
                                        1,
                                        static_cast<UINT>(firstIndex),
                                        static_cast<INT>(baseVertex),
                                        0);
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
    if (!impl->encoder || !impl->pipelineBound)
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

// The depth resolve drawn rather than asked for: the multisampled plane goes
// to PIXEL_SHADER_RESOURCE to be loaded from, the resolved buffer to
// DEPTH_WRITE to be written through its own DSV, and one triangle carries every
// pixel's furthest sample across. The pass that just ended has already bound
// its own root state, targets and viewport, and the next one binds its own
// again, so nothing set here has to be put back.
//
// The attachment returns to DEPTH_WRITE on the way out, as the API resolve
// leaves it; the resolved buffer stays in DEPTH_WRITE with depthState saying
// so, and whatever samples it next moves it from there.
void resolveDepthWithShader(CommandContext& commands, D3D12TextureData& texture)
{
    auto* list = commands.list.get();
    auto* context = commands.context;

    if (list == nullptr || context == nullptr || texture.msaaDepthSrv.gpu.ptr == 0
        || texture.resolvedDsv.ptr == 0)
        return;

    const auto& shader =
        getD3D12Shared().getDepthResolveShader(texture.depthHasStencil);

    if (!shader.isValid())
        return;

    transition(list,
               texture.depthResource.get(),
               texture.msaaDepthState,
               D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    texture.msaaDepthState = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;

    transitionDepthForUse(list, texture, D3D12_RESOURCE_STATE_DEPTH_WRITE);

    const auto desc = texture.resolvedDepthResource->GetDesc();

    D3D12_VIEWPORT viewport = {};
    viewport.Width = static_cast<float>(desc.Width);
    viewport.Height = static_cast<float>(desc.Height);
    viewport.MaxDepth = 1.f;

    const D3D12_RECT scissor = {
        0, 0, static_cast<LONG>(desc.Width), static_cast<LONG>(desc.Height)};

    ID3D12DescriptorHeap* heaps[] = {context->getTextureHeap(),
                                     context->getSamplerHeap()};
    list->SetDescriptorHeaps(2, heaps);
    list->SetGraphicsRootSignature(shader.rootSignature.get());
    list->SetGraphicsRootDescriptorTable(0, texture.msaaDepthSrv.gpu);
    list->SetPipelineState(shader.state.get());
    list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    list->OMSetRenderTargets(0, nullptr, FALSE, &texture.resolvedDsv);
    list->RSSetViewports(1, &viewport);
    list->RSSetScissorRects(1, &scissor);
    list->DrawInstanced(3, 1, 0, 0);

    transition(list,
               texture.depthResource.get(),
               D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
               D3D12_RESOURCE_STATE_DEPTH_WRITE);
    texture.msaaDepthState = D3D12_RESOURCE_STATE_DEPTH_WRITE;
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

    if (impl->encoder && impl->encoder->commands != nullptr && heap != nullptr)
        impl->encoder->commands->list->BeginQuery(
            heap, d3d12OcclusionType(mode), static_cast<UINT>(index));
}

void RenderPass::nativeEndOcclusionQuery(int index, OcclusionMode mode)
{
    auto* heap = static_cast<ID3D12QueryHeap*>(occlusionQueries->nativeQueries());

    if (impl->encoder && impl->encoder->commands != nullptr && heap != nullptr)
        impl->encoder->commands->list->EndQuery(
            heap, d3d12OcclusionType(mode), static_cast<UINT>(index));
}

// Each query the pass used, resolved into its eight bytes of the set's
// readback buffer, which rests in COPY_DEST as a readback heap's resources
// must. Only those: a query this pass did not begin keeps what an earlier
// frame resolved for it.
void RenderPass::resolveOcclusionQueries()
{
    if (occlusionQueries == nullptr || !impl->encoder
        || impl->encoder->commands == nullptr)
        return;

    auto* heap = static_cast<ID3D12QueryHeap*>(occlusionQueries->nativeQueries());
    auto* readback = static_cast<ID3D12Resource*>(occlusionQueries->nativeResults());

    if (heap == nullptr || readback == nullptr)
        return;

    for (const auto& used: usedQueries)
        impl->encoder->commands->list->ResolveQueryData(
            heap,
            d3d12OcclusionType(used.mode),
            static_cast<UINT>(used.index),
            1,
            readback,
            static_cast<UINT64>(used.index) * sizeof(std::uint64_t));
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
    if (impl->encoder && impl->encoder->commands != nullptr)
        for (auto* target: impl->encoder->resolveTargets)
            resolveMultisampledTarget(*impl->encoder->commands, *target);

    // Commands are recorded onto the frame's list, which submits when the
    // frame is destroyed; releasing the encoder marks the pass finished.
    impl->encoder.reset();
}
} // namespace eacp::GPU

#include <eacp/Core/Utils/WinInclude.h>

#include "RenderPass.h"

#include "../Buffer/Buffer.h"
#include "../Pipeline/RenderPipeline.h"
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
    if (!impl->encoder || range.buffer == nullptr)
        return;

    auto* data = static_cast<D3D12BufferData*>(range.buffer->nativeBuffer());

    if (data == nullptr || data->resource == nullptr || range.offset >= data->size)
        return;

    auto& commands = *impl->encoder->commands;
    transitionForUse(
        commands, *data, D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER);

    // A view starting part-way into the resource: the address moves up by the
    // offset and the size comes down by it, so the view still ends where the
    // buffer does and vertex zero is the byte at the offset.
    D3D12_VERTEX_BUFFER_VIEW view = {};
    view.BufferLocation = data->resource->GetGPUVirtualAddress() + range.offset;
    view.SizeInBytes = static_cast<UINT>(data->size - range.offset);
    view.StrideInBytes = strideForSlot(impl->encoder->strides, index);

    commands.list->IASetVertexBuffers(static_cast<UINT>(index), 1, &view);
}

void RenderPass::setFragmentTexture(const Texture& texture,
                                    int slot,
                                    TextureSampling)
{
    if (!impl->encoder || slot < 0 || slot >= maxTextureSlots)
        return;

    auto* data = static_cast<D3D12TextureData*>(texture.nativeTexture());

    if (data == nullptr || data->srv.gpu.ptr == 0)
        return;

    auto* list = impl->encoder->commands->list.get();

    // A render target an earlier pass on this frame drew into is still in
    // RENDER_TARGET, and this is where it goes back. A plain texture is already
    // in the state this asks for and the helper records nothing.
    transitionTextureForUse(list, *data, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);

    // Only the SRV: the sampler is a static sampler in the root signature, chosen
    // by the register the shader's sampler was emitted at. See TextureSampling.
    list->SetGraphicsRootDescriptorTable(renderTextureParam(slot), data->srv.gpu);
}

void RenderPass::setFragmentDepthTexture(const Texture& renderTarget,
                                         int slot,
                                         TextureSampling)
{
    if (!impl->encoder || slot < 0 || slot >= maxTextureSlots)
        return;

    auto* data = static_cast<D3D12TextureData*>(renderTarget.nativeTexture());

    if (data == nullptr || !data->hasSampleableDepth()
        || data->depthSrv.gpu.ptr == 0)
        return;

    auto* list = impl->encoder->commands->list.get();

    // The depth resource rests in DEPTH_WRITE, which is where the pass that drew
    // it left it and where the next pass to attach it expects to find it - so
    // this is the outward half of the pair, and Frame::beginPass records the
    // return. The two states are two views of one resource, which is why they
    // are one tracked value rather than a flag per view.
    transitionDepthForUse(list, *data, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);

    list->SetGraphicsRootDescriptorTable(renderTextureParam(slot),
                                         data->depthSrv.gpu);
}

namespace
{
// The address a stage's root SRV binds to, with the buffer moved into the state
// a shader read needs. A kernel that wrote this buffer left it in
// UNORDERED_ACCESS; the barrier here is what orders the draw behind that write.
D3D12_GPU_VIRTUAL_ADDRESS storageBufferAddress(CommandContext& commands,
                                               const Buffer& buffer)
{
    auto* data = static_cast<D3D12BufferData*>(buffer.nativeBuffer());

    if (data == nullptr || data->resource == nullptr)
        return 0;

    // The union of the two read states rather than one: the same buffer may be
    // bound to both stages, and asking for each in turn would barrier it back
    // and forth between two states that are both just "a shader is reading it".
    transitionForUse(commands,
                     *data,
                     D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE
                         | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);

    return data->resource->GetGPUVirtualAddress();
}
} // namespace

void RenderPass::setVertexStorageBuffer(const Buffer& buffer, int slot)
{
    if (!impl->encoder || slot < 0 || slot >= maxBufferSlots)
        return;

    auto& commands = *impl->encoder->commands;

    if (auto address = storageBufferAddress(commands, buffer))
        commands.list->SetGraphicsRootShaderResourceView(renderVertexSRVParam(slot),
                                                         address);
}

void RenderPass::setFragmentStorageBuffer(const Buffer& buffer, int slot)
{
    if (!impl->encoder || slot < 0 || slot >= maxBufferSlots)
        return;

    auto& commands = *impl->encoder->commands;

    if (auto address = storageBufferAddress(commands, buffer))
        commands.list->SetGraphicsRootShaderResourceView(renderPixelSRVParam(slot),
                                                         address);
}

void RenderPass::setVertexBytes(const void* data, std::size_t bytes, int slot)
{
    if (!impl->encoder || slot < 0 || slot >= maxUniformSlots)
        return;

    auto& commands = *impl->encoder->commands;
    auto address = commands.context->uploadConstants(commands, data, bytes);

    if (address != 0)
        commands.list->SetGraphicsRootConstantBufferView(renderVertexCBVParam(slot),
                                                         address);
}

void RenderPass::setFragmentBytes(const void* data, std::size_t bytes, int slot)
{
    if (!impl->encoder || slot < 0 || slot >= maxUniformSlots)
        return;

    auto& commands = *impl->encoder->commands;
    auto address = commands.context->uploadConstants(commands, data, bytes);

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
    if (indices.buffer == nullptr)
        return false;

    auto* data = static_cast<D3D12BufferData*>(indices.buffer->nativeBuffer());

    if (data == nullptr || data->resource == nullptr || indices.offset >= data->size)
        return false;

    transitionForUse(commands, *data, D3D12_RESOURCE_STATE_INDEX_BUFFER);

    D3D12_INDEX_BUFFER_VIEW view = {};
    view.BufferLocation = data->resource->GetGPUVirtualAddress() + indices.offset;
    view.SizeInBytes = static_cast<UINT>(data->size - indices.offset);
    view.Format =
        format == IndexFormat::UInt16 ? DXGI_FORMAT_R16_UINT : DXGI_FORMAT_R32_UINT;

    commands.list->IASetIndexBuffer(&view);

    return true;
}
} // namespace

void RenderPass::drawIndexed(const Buffer& indices,
                             int indexCount,
                             IndexFormat format,
                             int firstIndex,
                             int baseVertex)
{
    drawIndexed(BufferRange::of(indices), indexCount, format, firstIndex, baseVertex);
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

void RenderPass::end()
{
    // Before the encoder goes, so a batching renderer's queued draws still get
    // recorded. See RenderPass::Participant.
    drainParticipants();

    // After them, so what a participant flushed is inside the pass being timed
    // rather than after it.
    if (impl->encoder)
        endTimedPass(*impl->encoder);

    // Commands are recorded onto the frame's list, which submits when the
    // frame is destroyed; releasing the encoder marks the pass finished.
    impl->encoder.reset();
}
} // namespace eacp::GPU

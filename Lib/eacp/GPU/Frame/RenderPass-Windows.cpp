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
    const auto left = std::clamp(
        static_cast<int>(std::floor(rect.x)), 0, impl->targetWidth);
    const auto top = std::clamp(
        static_cast<int>(std::floor(rect.y)), 0, impl->targetHeight);
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

void RenderPass::setVertexBuffer(const Buffer& buffer, int index)
{
    if (!impl->encoder)
        return;

    auto* data = static_cast<D3D12BufferData*>(buffer.nativeBuffer());

    if (data == nullptr || data->resource == nullptr)
        return;

    auto& commands = *impl->encoder->commands;
    transitionForUse(
        commands, *data, D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER);

    D3D12_VERTEX_BUFFER_VIEW view = {};
    view.BufferLocation = data->resource->GetGPUVirtualAddress();
    view.SizeInBytes = static_cast<UINT>(data->size);
    view.StrideInBytes = strideForSlot(impl->encoder->strides, index);

    commands.list->IASetVertexBuffers(static_cast<UINT>(index), 1, &view);
}

void RenderPass::setFragmentTexture(const Texture& texture, int slot)
{
    if (!impl->encoder || slot < 0 || slot >= maxTextureSlots)
        return;

    auto* data = static_cast<D3D12TextureData*>(texture.nativeTexture());

    if (data == nullptr || data->srv.gpu.ptr == 0 || data->sampler.gpu.ptr == 0)
        return;

    auto* list = impl->encoder->commands->list.get();
    list->SetGraphicsRootDescriptorTable(renderTextureParam(slot), data->srv.gpu);
    list->SetGraphicsRootDescriptorTable(renderSamplerParam(slot),
                                         data->sampler.gpu);
}

void RenderPass::setVertexBytes(const void* data, std::size_t bytes, int slot)
{
    if (!impl->encoder || slot < 0 || slot >= maxUniformSlots)
        return;

    auto& commands = *impl->encoder->commands;
    auto address = getD3D12Context().uploadConstants(commands, data, bytes);

    if (address != 0)
        commands.list->SetGraphicsRootConstantBufferView(renderVertexCBVParam(slot),
                                                         address);
}

void RenderPass::setFragmentBytes(const void* data, std::size_t bytes, int slot)
{
    if (!impl->encoder || slot < 0 || slot >= maxUniformSlots)
        return;

    auto& commands = *impl->encoder->commands;
    auto address = getD3D12Context().uploadConstants(commands, data, bytes);

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

void RenderPass::drawIndexed(const Buffer& indices,
                             int indexCount,
                             IndexFormat format,
                             int firstIndex)
{
    if (!impl->encoder || !impl->pipelineBound)
        return;

    auto* data = static_cast<D3D12BufferData*>(indices.nativeBuffer());

    if (data == nullptr || data->resource == nullptr)
        return;

    auto& commands = *impl->encoder->commands;
    transitionForUse(commands, *data, D3D12_RESOURCE_STATE_INDEX_BUFFER);

    D3D12_INDEX_BUFFER_VIEW view = {};
    view.BufferLocation = data->resource->GetGPUVirtualAddress();
    view.SizeInBytes = static_cast<UINT>(data->size);
    view.Format =
        format == IndexFormat::UInt16 ? DXGI_FORMAT_R16_UINT : DXGI_FORMAT_R32_UINT;

    commands.list->IASetIndexBuffer(&view);
    commands.list->DrawIndexedInstanced(
        static_cast<UINT>(indexCount), 1, static_cast<UINT>(firstIndex), 0, 0);
}

void RenderPass::drawIndexedInstanced(const Buffer& indices,
                                      int indexCount,
                                      int instanceCount,
                                      IndexFormat format,
                                      int firstIndex,
                                      int firstInstance)
{
    if (!impl->encoder || !impl->pipelineBound)
        return;

    auto* data = static_cast<D3D12BufferData*>(indices.nativeBuffer());

    if (data == nullptr || data->resource == nullptr)
        return;

    auto& commands = *impl->encoder->commands;
    transitionForUse(commands, *data, D3D12_RESOURCE_STATE_INDEX_BUFFER);

    D3D12_INDEX_BUFFER_VIEW view = {};
    view.BufferLocation = data->resource->GetGPUVirtualAddress();
    view.SizeInBytes = static_cast<UINT>(data->size);
    view.Format =
        format == IndexFormat::UInt16 ? DXGI_FORMAT_R16_UINT : DXGI_FORMAT_R32_UINT;

    commands.list->IASetIndexBuffer(&view);
    commands.list->DrawIndexedInstanced(static_cast<UINT>(indexCount),
                                        static_cast<UINT>(instanceCount),
                                        static_cast<UINT>(firstIndex),
                                        0,
                                        static_cast<UINT>(firstInstance));
}

void RenderPass::end()
{
    // Commands are recorded onto the frame's list, which submits when the
    // frame is destroyed; releasing the encoder marks the pass finished.
    impl->encoder.reset();
}
} // namespace eacp::GPU

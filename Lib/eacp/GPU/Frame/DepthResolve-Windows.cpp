#include <eacp/Core/Utils/WinInclude.h>

#include "../Windows/D3D12Context.h"
#include "../Windows/D3D12Types.h"

namespace eacp::GPU
{
static constexpr auto depthResolveTriangleVertexCount = 3u;

static bool canResolveDepthWithShader(const CommandContext& commands,
                                      const D3D12TextureData& texture)
{
    return commands.list != nullptr && commands.context != nullptr
           && texture.msaaDepthSrv.gpu.ptr != 0 && texture.resolvedDsv.ptr != 0;
}

static void moveDepthPlanesForShaderResolve(ID3D12GraphicsCommandList* list,
                                            D3D12TextureData& texture)
{
    transition(list,
               texture.depthResource.get(),
               texture.msaaDepthState,
               D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    texture.msaaDepthState = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;

    transitionDepthForUse(list, texture, D3D12_RESOURCE_STATE_DEPTH_WRITE);
}

static void returnMultisampledDepthToDepthWrite(ID3D12GraphicsCommandList* list,
                                                D3D12TextureData& texture)
{
    transition(list,
               texture.depthResource.get(),
               D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
               D3D12_RESOURCE_STATE_DEPTH_WRITE);
    texture.msaaDepthState = D3D12_RESOURCE_STATE_DEPTH_WRITE;
}

static void setDepthResolveTargetArea(ID3D12GraphicsCommandList* list,
                                      const D3D12TextureData& texture)
{
    const auto desc = texture.resolvedDepthResource->GetDesc();

    D3D12_VIEWPORT viewport = {};
    viewport.Width = static_cast<float>(desc.Width);
    viewport.Height = static_cast<float>(desc.Height);
    viewport.MaxDepth = 1.f;

    const D3D12_RECT scissor = {
        0, 0, static_cast<LONG>(desc.Width), static_cast<LONG>(desc.Height)};

    list->RSSetViewports(1, &viewport);
    list->RSSetScissorRects(1, &scissor);
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
    if (!canResolveDepthWithShader(commands, texture))
        return;

    auto* list = commands.list.get();
    auto* context = commands.context;

    const auto& shader =
        getD3D12Shared().getDepthResolveShader(texture.depthHasStencil);

    if (!shader.isValid())
        return;

    moveDepthPlanesForShaderResolve(list, texture);

    ID3D12DescriptorHeap* heaps[] = {context->getTextureHeap(),
                                     context->getSamplerHeap()};
    list->SetDescriptorHeaps(2, heaps);
    list->SetGraphicsRootSignature(shader.rootSignature.get());
    list->SetGraphicsRootDescriptorTable(0, texture.msaaDepthSrv.gpu);
    list->SetPipelineState(shader.state.get());
    list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    list->OMSetRenderTargets(0, nullptr, FALSE, &texture.resolvedDsv);
    setDepthResolveTargetArea(list, texture);
    list->DrawInstanced(depthResolveTriangleVertexCount, 1, 0, 0);

    returnMultisampledDepthToDepthWrite(list, texture);
}
} // namespace eacp::GPU

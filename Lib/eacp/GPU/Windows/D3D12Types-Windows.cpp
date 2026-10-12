#include <eacp/Core/Utils/WinInclude.h>

#include "D3D12Types.h"

#include "../Texture/Sampler.h"

#include <eacp/Core/Utils/Logging.h>

namespace eacp::GPU
{
bool hasSamplerTables()
{
    return !getD3D12Shared().getDriverQuirks().samplerTablesIgnoreOffset;
}

UINT strideForSlot(const Vector<UINT>& strides, int slot)
{
    if (slot >= 0 && slot < strides.size())
        return strides[slot];
    if (!strides.empty())
        return strides[0];
    return 0;
}

D3D12_GPU_DESCRIPTOR_HANDLE perBindSamplerDescriptor(const Sampler& sampler)
{
    if (!hasSamplerTables())
    {
        static auto reported = false;

        if (!reported)
        {
            reported = true;
            LOG("D3D12: this driver ignores sampler table offsets; per-bind "
                "samplers are dropped and their registers hold a fixed "
                "linear/repeat static sampler. See SAMPLERS.md.");
        }

        return {};
    }

    auto* data = static_cast<D3D12SamplerData*>(sampler.nativeSampler());

    if (data == nullptr)
        return {};

    return data->slot.gpu;
}

DXGI_FORMAT depthAttachmentFormat(bool withStencil)
{
    return withStencil ? DXGI_FORMAT_D32_FLOAT_S8X24_UINT : DXGI_FORMAT_D32_FLOAT;
}

DXGI_FORMAT depthResourceFormat(bool withStencil, bool sampleable)
{
    if (!sampleable)
        return depthAttachmentFormat(withStencil);

    return withStencil ? DXGI_FORMAT_R32G8X24_TYPELESS : DXGI_FORMAT_R32_TYPELESS;
}

DXGI_FORMAT depthShaderResourceFormat(bool withStencil)
{
    return withStencil ? DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS
                       : DXGI_FORMAT_R32_FLOAT;
}

DXGI_FORMAT depthResolveFormat(bool withStencil)
{
    return withStencil ? DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS
                       : DXGI_FORMAT_R32_FLOAT;
}

D3D12_CLEAR_FLAGS depthClearFlags(bool withStencil)
{
    return withStencil ? static_cast<D3D12_CLEAR_FLAGS>(D3D12_CLEAR_FLAG_DEPTH
                                                        | D3D12_CLEAR_FLAG_STENCIL)
                       : D3D12_CLEAR_FLAG_DEPTH;
}

D3D12_GPU_DESCRIPTOR_HANDLE D3D12TextureData::depthSliceView(int slice) const
{
    if (slice == 0 && depthSliceSrvs.empty())
        return depthSrv.gpu;

    if (slice < 0 || slice >= static_cast<int>(depthSliceSrvs.size()))
        return {};

    return depthSliceSrvs[static_cast<std::size_t>(slice)].gpu;
}

void transitionDepthForUse(ID3D12GraphicsCommandList* list,
                           D3D12TextureData& texture,
                           D3D12_RESOURCE_STATES target)
{
    auto* resource = texture.sampledDepthResource();

    if (resource == nullptr || !texture.hasSampleableDepth()
        || texture.depthState == target)
        return;

    D3D12_RESOURCE_BARRIER barrier = {};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = resource;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore = texture.depthState;
    barrier.Transition.StateAfter = target;
    list->ResourceBarrier(1, &barrier);

    texture.depthState = target;
}

void transitionForUse(CommandContext& commands,
                      D3D12BufferData& buffer,
                      D3D12_RESOURCE_STATES target)
{
    // An upload-heap buffer is already in the one state it may ever be in, and
    // GENERIC_READ covers every way a draw reads one - vertices, indices,
    // constants, a shader resource. Nothing to record, which is the point.
    if (buffer.uploadHeap)
        return;

    if (buffer.recordingId != commands.recordingId)
    {
        buffer.recordingId = commands.recordingId;
        buffer.state = target;
        return;
    }

    if (buffer.state == target)
        return;

    D3D12_RESOURCE_BARRIER barrier = {};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = buffer.resource.get();
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore = buffer.state;
    barrier.Transition.StateAfter = target;
    commands.list->ResourceBarrier(1, &barrier);

    buffer.state = target;
}

void transitionTextureForUse(ID3D12GraphicsCommandList* list,
                             D3D12TextureData& texture,
                             D3D12_RESOURCE_STATES target)
{
    if (texture.resource == nullptr || texture.state == target)
        return;

    D3D12_RESOURCE_BARRIER barrier = {};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = texture.resource.get();
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore = texture.state;
    barrier.Transition.StateAfter = target;
    list->ResourceBarrier(1, &barrier);

    texture.state = target;
}

void bindComputeRootState(D3D12Context& context, ID3D12GraphicsCommandList* list)
{
    ID3D12DescriptorHeap* heaps[] = {context.getTextureHeap(),
                                     context.getSamplerHeap()};
    list->SetDescriptorHeaps(2, heaps);
    list->SetComputeRootSignature(context.getComputeRootSignature());

    // Resource Binding Tier 1 hardware requires every descriptor table the root
    // signature declares to be populated before a dispatch, even the ones the
    // kernel never touches - an unset table drops the dispatch rather than
    // failing loudly. The signature is shared and declares maxTextureSlots of
    // each; setInputTexture / setOutputTexture overwrite the slots that carry a
    // real texture. Same rule, and the same fix, as the render path's.
    const auto nullSrv = context.getNullTextureDescriptor();
    const auto nullUav = context.getNullTextureUAVDescriptor();

    if (nullSrv.ptr == 0 || nullUav.ptr == 0)
        return;

    for (auto slot = 0; slot < maxTextureSlots; ++slot)
    {
        list->SetComputeRootDescriptorTable(computeTextureSRVParam(slot), nullSrv);
        list->SetComputeRootDescriptorTable(computeTextureUAVParam(slot), nullUav);
    }

    const auto nullSampler = context.getNullSamplerDescriptor();

    if (!hasSamplerTables() || nullSampler.ptr == 0)
        return;

    for (auto slot = 0; slot < maxTextureSlots; ++slot)
        list->SetComputeRootDescriptorTable(computeSamplerParam(slot), nullSampler);
}

void transition(ID3D12GraphicsCommandList* list,
                ID3D12Resource* resource,
                D3D12_RESOURCE_STATES before,
                D3D12_RESOURCE_STATES after)
{
    D3D12_RESOURCE_BARRIER barrier = {};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = resource;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore = before;
    barrier.Transition.StateAfter = after;
    list->ResourceBarrier(1, &barrier);
}

void resolveMultisampledTarget(CommandContext& commands, D3D12TextureData& texture)
{
    auto* list = commands.list.get();

    if (!texture.isMultisampled() || texture.resource == nullptr)
        return;

    transition(list,
               texture.msaaResource.get(),
               texture.msaaState,
               D3D12_RESOURCE_STATE_RESOLVE_SOURCE);
    texture.msaaState = D3D12_RESOURCE_STATE_RESOLVE_SOURCE;

    transitionTextureForUse(list, texture, D3D12_RESOURCE_STATE_RESOLVE_DEST);

    list->ResolveSubresource(texture.resource.get(),
                             0,
                             texture.msaaResource.get(),
                             0,
                             texture.colorFormat);

    transition(list,
               texture.msaaResource.get(),
               D3D12_RESOURCE_STATE_RESOLVE_SOURCE,
               D3D12_RESOURCE_STATE_RENDER_TARGET);
    texture.msaaState = D3D12_RESOURCE_STATE_RENDER_TARGET;

    if (texture.resolvedDepthResource == nullptr)
        return;

    if (getD3D12Shared().getDriverQuirks().noDepthResolve)
    {
        resolveDepthWithShader(commands, texture);
        return;
    }

    winrt::com_ptr<ID3D12GraphicsCommandList1> list1;

    if (FAILED(list->QueryInterface(__uuidof(ID3D12GraphicsCommandList1),
                                    list1.put_void())))
        return;

    transition(list,
               texture.depthResource.get(),
               texture.msaaDepthState,
               D3D12_RESOURCE_STATE_RESOLVE_SOURCE);
    texture.msaaDepthState = D3D12_RESOURCE_STATE_RESOLVE_SOURCE;

    transitionDepthForUse(list, texture, D3D12_RESOURCE_STATE_RESOLVE_DEST);

    list1->ResolveSubresourceRegion(texture.resolvedDepthResource.get(),
                                    0,
                                    0,
                                    0,
                                    texture.depthResource.get(),
                                    0,
                                    nullptr,
                                    depthResolveFormat(texture.depthHasStencil),
                                    D3D12_RESOLVE_MODE_MAX);

    transition(list,
               texture.depthResource.get(),
               D3D12_RESOURCE_STATE_RESOLVE_SOURCE,
               D3D12_RESOURCE_STATE_DEPTH_WRITE);
    texture.msaaDepthState = D3D12_RESOURCE_STATE_DEPTH_WRITE;
}
} // namespace eacp::GPU

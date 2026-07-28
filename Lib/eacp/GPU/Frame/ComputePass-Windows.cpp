#include <eacp/Core/Utils/WinInclude.h>

#include "ComputePass.h"

#include "../Buffer/Buffer.h"
#include "../Pipeline/ComputePipeline.h"
#include "../Windows/D3D12Types.h"

// Windows/D3D12 backend. Records onto the command buffer's recording via the
// D3D12ComputeEncoder. Buffers bind as root descriptors by GPU address (no
// descriptor heap involved); textures cannot - a root descriptor is a buffer
// view and nothing else - so those bind through single-descriptor tables, out
// of the heaps beginCompute bound. Uniforms upload into a transient buffer
// bound as a root CBV. A UAV barrier after every dispatch orders chained
// kernels, and covers a texture written by one and read by the next exactly as
// it covers a buffer.

namespace eacp::GPU
{
struct ComputePass::Native
{
    explicit Native(void* encoderHandle)
        : encoder(static_cast<D3D12ComputeEncoder*>(encoderHandle))
    {
    }

    std::unique_ptr<D3D12ComputeEncoder> encoder;
};

ComputePass::ComputePass(void* encoder)
    : impl(encoder)
{
}

ComputePass::~ComputePass()
{
    end();
}

void ComputePass::setPipeline(const ComputePipeline& pipeline)
{
    if (!impl->encoder)
        return;

    if (auto* state = static_cast<ID3D12PipelineState*>(pipeline.nativeState()))
        impl->encoder->commands->list->SetPipelineState(state);
}

void ComputePass::setInputBuffer(const Buffer& buffer, int slot)
{
    if (!impl->encoder || slot < 0 || slot >= maxBufferSlots)
        return;

    auto* data = static_cast<D3D12BufferData*>(buffer.nativeBuffer());

    if (data == nullptr || data->resource == nullptr)
        return;

    auto& commands = *impl->encoder->commands;
    transitionForUse(
        commands, *data, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    commands.list->SetComputeRootShaderResourceView(
        computeSRVParam(slot), data->resource->GetGPUVirtualAddress());
}

void ComputePass::setOutputBuffer(const Buffer& buffer, int slot)
{
    if (!impl->encoder || slot < 0 || slot >= maxBufferSlots)
        return;

    auto* data = static_cast<D3D12BufferData*>(buffer.nativeBuffer());

    if (data == nullptr || data->resource == nullptr)
        return;

    auto& commands = *impl->encoder->commands;
    transitionForUse(commands, *data, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    commands.list->SetComputeRootUnorderedAccessView(
        computeUAVParam(slot), data->resource->GetGPUVirtualAddress());
}

void ComputePass::setInputTexture(const Texture& texture, int slot, TextureSampling)
{
    if (!impl->encoder || slot < 0 || slot >= maxTextureSlots)
        return;

    auto* data = static_cast<D3D12TextureData*>(texture.nativeTexture());

    if (data == nullptr || data->srv.gpu.ptr == 0)
        return;

    auto* list = impl->encoder->commands->list.get();

    // A texture an earlier kernel wrote is still in UNORDERED_ACCESS, and this
    // is where it comes back from. Only the SRV is bound: the sampler is a
    // static sampler in the compute root signature, picked by the register the
    // shader's sampler was emitted at. See TextureSampling.
    transitionTextureForUse(
        list, *data, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    list->SetComputeRootDescriptorTable(computeTextureSRVParam(slot), data->srv.gpu);
}

void ComputePass::setOutputTexture(const Texture& texture, int slot)
{
    if (!impl->encoder || slot < 0 || slot >= maxTextureSlots)
        return;

    auto* data = static_cast<D3D12TextureData*>(texture.nativeTexture());

    if (data == nullptr || !data->isComputeWritable() || data->uav.gpu.ptr == 0)
        return;

    auto* list = impl->encoder->commands->list.get();

    transitionTextureForUse(list, *data, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    list->SetComputeRootDescriptorTable(computeTextureUAVParam(slot), data->uav.gpu);
}

void ComputePass::setBytes(const void* data, std::size_t bytes, int slot)
{
    if (!impl->encoder || slot < 0 || slot >= maxUniformSlots)
        return;

    auto& commands = *impl->encoder->commands;
    auto address = getD3D12Context().uploadConstants(commands, data, bytes);

    if (address != 0)
        commands.list->SetComputeRootConstantBufferView(computeCBVParam(slot),
                                                        address);
}

namespace
{
// Orders a dispatch's UAV writes against any later read or write of the same
// resources in this recording (chained kernels, readback copies).
void barrierAfterDispatch(ID3D12GraphicsCommandList* list)
{
    D3D12_RESOURCE_BARRIER barrier = {};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    list->ResourceBarrier(1, &barrier);
}
} // namespace

void ComputePass::dispatch(int count)
{
    if (!impl->encoder || count <= 0)
        return;

    auto groups =
        (static_cast<UINT>(count) + threadGroupWidth - 1) / threadGroupWidth;

    auto* list = impl->encoder->commands->list.get();
    list->Dispatch(groups, 1, 1);
    barrierAfterDispatch(list);
}

void ComputePass::dispatch(int width, int height)
{
    if (!impl->encoder || width <= 0 || height <= 0)
        return;

    auto size = static_cast<UINT>(threadGroupSize2D);
    auto groupsX = (static_cast<UINT>(width) + size - 1) / size;
    auto groupsY = (static_cast<UINT>(height) + size - 1) / size;

    auto* list = impl->encoder->commands->list.get();
    list->Dispatch(groupsX, groupsY, 1);
    barrierAfterDispatch(list);
}

void ComputePass::end()
{
    if (impl->encoder)
        endTimedPass(*impl->encoder);

    impl->encoder.reset();
}
} // namespace eacp::GPU

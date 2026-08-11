#include <eacp/Core/Utils/WinInclude.h>

#include "ComputePass.h"

#include "../Buffer/Buffer.h"
#include "../Pipeline/ComputePipeline.h"
#include "../Windows/D3D12Types.h"

// Buffers bind as root descriptors by GPU address; textures cannot (a root
// descriptor is a buffer view), so they bind through single-descriptor tables.

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

    // Only the SRV: samplers are static in the compute root signature, picked
    // by the register the shader emitted them at. See TextureSampling.
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

// Indirect arguments are only legal to read from INDIRECT_ARGUMENT, so the
// buffer a kernel wrote as a UAV needs a transition Metal has no equivalent of.
void ComputePass::dispatchIndirect(const Buffer& arguments, int offsetInBytes)
{
    if (!impl->encoder || offsetInBytes < 0)
        return;

    auto* data = static_cast<D3D12BufferData*>(arguments.nativeBuffer());

    if (data == nullptr || data->resource == nullptr)
        return;

    auto* signature = getD3D12Context().getDispatchSignature();

    if (signature == nullptr)
        return;

    auto& commands = *impl->encoder->commands;
    transitionForUse(commands, *data, D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT);

    auto* list = commands.list.get();
    list->ExecuteIndirect(signature,
                          1,
                          data->resource.get(),
                          static_cast<UINT64>(offsetInBytes),
                          nullptr,
                          0);
    barrierAfterDispatch(list);
}

void ComputePass::end()
{
    if (impl->encoder)
        endTimedPass(*impl->encoder);

    impl->encoder.reset();
}
} // namespace eacp::GPU

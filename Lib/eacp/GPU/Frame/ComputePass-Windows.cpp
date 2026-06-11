#include <eacp/Core/Utils/WinInclude.h>

#include "ComputePass.h"

#include "../Buffer/Buffer.h"
#include "../Pipeline/ComputePipeline.h"
#include "../Windows/D3D12Types.h"

#include <memory>

// Windows/D3D12 backend. Records onto the command buffer's recording via the
// D3D12ComputeEncoder. Buffers bind as root descriptors by GPU address (no
// descriptor heap involved); uniforms upload into a transient buffer bound as
// a root CBV. A UAV barrier after every dispatch orders chained kernels.

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

void ComputePass::dispatch(int count)
{
    if (!impl->encoder || count <= 0)
        return;

    auto groups =
        (static_cast<UINT>(count) + threadGroupWidth - 1) / threadGroupWidth;

    auto* list = impl->encoder->commands->list.get();
    list->Dispatch(groups, 1, 1);

    // Orders this dispatch's UAV writes against any later read or write of
    // the same buffers in this recording (chained kernels, readback copies).
    D3D12_RESOURCE_BARRIER barrier = {};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    list->ResourceBarrier(1, &barrier);
}

void ComputePass::end()
{
    impl->encoder.reset();
}
} // namespace eacp::GPU

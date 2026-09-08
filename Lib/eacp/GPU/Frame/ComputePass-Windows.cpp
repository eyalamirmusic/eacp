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
// it covers a buffer. A concurrent pass drops that per-dispatch barrier -
// dispatches on a list overlap unless something says otherwise - and records
// one where barrier() asks and one more as the pass ends.

namespace eacp::GPU
{
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

struct ComputePass::Native
{
    Native(void* encoderHandle, DispatchOrder dispatchOrder)
        : encoder(static_cast<D3D12ComputeEncoder*>(encoderHandle))
        , order(dispatchOrder)
    {
    }

    bool isConcurrent() const { return order == DispatchOrder::Concurrent; }

    void orderAfterDispatch(ID3D12GraphicsCommandList* list) const
    {
        if (!isConcurrent())
            barrierAfterDispatch(list);
    }

    void recordBarrier() const
    {
        if (encoder != nullptr && encoder->commands != nullptr)
            barrierAfterDispatch(encoder->commands->list.get());
    }

    std::unique_ptr<D3D12ComputeEncoder> encoder;
    DispatchOrder order = DispatchOrder::Serial;
};

ComputePass::ComputePass(void* encoder, DispatchOrder order)
    : impl(encoder, order)
{
}

ComputePass::~ComputePass()
{
    end();
}

void ComputePass::setPipeline(const ComputePipeline& pipeline)
{
    boundGroup = pipeline.threadGroupShape();

    if (!impl->encoder)
        return;

    if (auto* state = static_cast<ID3D12PipelineState*>(pipeline.nativeState()))
        impl->encoder->commands->list->SetPipelineState(state);
}

namespace
{
D3D12_GPU_VIRTUAL_ADDRESS rootDescriptorAddress(D3D12BufferData* data,
                                                const BufferRange& range)
{
    if (data == nullptr || data->resource == nullptr || range.offset < 0
        || (UINT64) range.offset >= data->size)
        return 0;

    return data->resource->GetGPUVirtualAddress() + (UINT64) range.offset;
}
} // namespace

void ComputePass::setInputBuffer(const Buffer& buffer, int slot)
{
    setInputBuffer(BufferRange::of(buffer), slot);
}

void ComputePass::setInputBuffer(const BufferRange& range, int slot)
{
    if (!impl->encoder || slot < 0 || slot >= maxBufferSlots
        || range.buffer == nullptr)
        return;

    auto* data = static_cast<D3D12BufferData*>(range.buffer->nativeBuffer());
    auto address = rootDescriptorAddress(data, range);

    if (address == 0)
        return;

    auto& commands = *impl->encoder->commands;
    transitionForUse(
        commands, *data, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    commands.list->SetComputeRootShaderResourceView(computeSRVParam(slot), address);
}

void ComputePass::setOutputBuffer(const Buffer& buffer, int slot)
{
    setOutputBuffer(BufferRange::of(buffer), slot);
}

void ComputePass::setOutputBuffer(const BufferRange& range, int slot)
{
    if (!impl->encoder || slot < 0 || slot >= maxBufferSlots
        || range.buffer == nullptr)
        return;

    auto* data = static_cast<D3D12BufferData*>(range.buffer->nativeBuffer());
    auto address = rootDescriptorAddress(data, range);

    if (address == 0)
        return;

    auto& commands = *impl->encoder->commands;
    transitionForUse(commands, *data, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    commands.list->SetComputeRootUnorderedAccessView(computeUAVParam(slot), address);
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

void ComputePass::setBytes(const void* data, int bytes, int slot)
{
    if (!impl->encoder || slot < 0 || slot >= maxUniformSlots)
        return;

    auto& commands = *impl->encoder->commands;
    auto address =
        commands.context->uploadConstants(commands, data, (std::size_t) bytes);

    if (address != 0)
        commands.list->SetComputeRootConstantBufferView(computeCBVParam(slot),
                                                        address);
}

void ComputePass::dispatch(int count)
{
    if (!impl->encoder || count <= 0)
        return;

    auto width = static_cast<UINT>(groupFor1D().x);
    auto groups = (static_cast<UINT>(count) + width - 1) / width;

    auto* list = impl->encoder->commands->list.get();
    list->Dispatch(groups, 1, 1);
    impl->orderAfterDispatch(list);
}

void ComputePass::dispatch(int width, int height)
{
    if (!impl->encoder || width <= 0 || height <= 0)
        return;

    auto group = groupFor2D();
    auto sizeX = static_cast<UINT>(group.x);
    auto sizeY = static_cast<UINT>(group.y);
    auto groupsX = (static_cast<UINT>(width) + sizeX - 1) / sizeX;
    auto groupsY = (static_cast<UINT>(height) + sizeY - 1) / sizeY;

    auto* list = impl->encoder->commands->list.get();
    list->Dispatch(groupsX, groupsY, 1);
    impl->orderAfterDispatch(list);
}

void ComputePass::dispatch(int width, int height, int depth)
{
    if (!impl->encoder || width <= 0 || height <= 0 || depth <= 0)
        return;

    auto group = groupFor3D();
    auto sizeX = static_cast<UINT>(group.x);
    auto sizeY = static_cast<UINT>(group.y);
    auto sizeZ = static_cast<UINT>(group.z);
    auto groupsX = (static_cast<UINT>(width) + sizeX - 1) / sizeX;
    auto groupsY = (static_cast<UINT>(height) + sizeY - 1) / sizeY;
    auto groupsZ = (static_cast<UINT>(depth) + sizeZ - 1) / sizeZ;

    auto* list = impl->encoder->commands->list.get();
    list->Dispatch(groupsX, groupsY, groupsZ);
    impl->orderAfterDispatch(list);
}

// The grid comes out of the buffer; the threadgroup size is baked into the
// shader's [numthreads] and is not part of the arguments, which is why
// D3D12_DISPATCH_ARGUMENTS holds only the three counts - the same three
// DispatchArguments holds, at the same size and in the same order.
//
// The buffer needs a state of its own here. An earlier kernel wrote it as a
// UAV, and a resource is only legal to read as indirect arguments from
// INDIRECT_ARGUMENT - a transition Metal has no equivalent of and the reason
// this is not simply the same three lines twice. That transition is only a
// transition, so in a concurrent pass the writer's UAV work is ordered against
// it by hand first.
void ComputePass::dispatchIndirect(const Buffer& arguments, int offsetInBytes)
{
    if (!impl->encoder || offsetInBytes < 0
        || offsetInBytes > arguments.size() - (int) sizeof(DispatchArguments))
        return;

    auto* data = static_cast<D3D12BufferData*>(arguments.nativeBuffer());

    if (data == nullptr || data->resource == nullptr)
        return;

    auto& commands = *impl->encoder->commands;
    auto* signature = commands.context->getDispatchSignature();

    if (signature == nullptr)
        return;

    auto* list = commands.list.get();

    if (impl->isConcurrent())
        impl->recordBarrier();

    transitionForUse(commands, *data, D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT);

    list->ExecuteIndirect(signature,
                          1,
                          data->resource.get(),
                          static_cast<UINT64>(offsetInBytes),
                          nullptr,
                          0);
    impl->orderAfterDispatch(list);
}

void ComputePass::barrier()
{
    if (impl->isConcurrent())
        impl->recordBarrier();
}

// A concurrent pass owes the rest of the recording what the per-dispatch
// barriers owed it in a serial one, so the last dispatches are ordered here
// against whatever the next pass or a readback copy does.
void ComputePass::end()
{
    if (impl->encoder)
    {
        if (impl->isConcurrent())
            impl->recordBarrier();

        endTimedPass(*impl->encoder);
    }

    impl->encoder.reset();
}
} // namespace eacp::GPU

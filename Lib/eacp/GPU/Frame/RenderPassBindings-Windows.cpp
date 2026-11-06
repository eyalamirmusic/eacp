#include <eacp/Core/Utils/WinInclude.h>

#include "RenderPassBindings-Windows.h"

#include "RenderPassRegions.h"

#include "../Buffer/Buffer.h"
#include "../Texture/Sampler.h"
#include "../Texture/Texture.h"

namespace eacp::GPU
{
static const auto renderPassAnyShaderRead =
    D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE
    | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;

// A vertex read needs NON_PIXEL_SHADER_RESOURCE, and asks for both read states
// so that a fragment bind of the same texture in the same draw finds its own
// already there rather than barriering the vertex one away.
static bool hasRenderPassReadState(D3D12_RESOURCE_STATES state,
                                   D3D12_RESOURCE_STATES wanted)
{
    return (state & wanted) == wanted;
}

static D3D12_RESOURCE_STATES getRenderPassReadState(ShaderStage stage)
{
    if (stage == ShaderStage::Vertex)
        return renderPassAnyShaderRead;

    return D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
}

static bool isTextureSlotInRange(int slot)
{
    return slot >= 0 && slot < maxTextureSlots;
}

static bool canBindTextureSlot(const D3D12Encoder* encoder, int slot)
{
    return encoder != nullptr && isTextureSlotInRange(slot);
}

static D3D12BufferData* getBufferDataAtOffset(const BufferRange& range)
{
    if (range.buffer == nullptr || range.offset < 0)
        return nullptr;

    auto* data = static_cast<D3D12BufferData*>(range.buffer->nativeBuffer());

    if (data == nullptr || data->resource == nullptr)
        return nullptr;

    if (!isOffsetInsideBuffer(range.offset, data->size))
        return nullptr;

    return data;
}

static D3D12_GPU_VIRTUAL_ADDRESS getAddressAtOffset(const D3D12BufferData& data,
                                                    std::int64_t offset)
{
    return data.resource->GetGPUVirtualAddress() + (UINT64) offset;
}

static UINT getSizeFromOffset(const D3D12BufferData& data, std::int64_t offset)
{
    return static_cast<UINT>(data.size - (UINT64) offset);
}

static bool hasSampleableDepthView(const D3D12TextureData* data)
{
    return data != nullptr && data->hasSampleableDepth();
}

// The depth resource rests in DEPTH_WRITE, which is where the pass that drew
// it left it and where the next pass to attach it expects to find it - so
// this is the outward half of the pair, and Frame::beginPass records the
// return. The two states are two views of one resource, which is why they
// are one tracked value rather than a flag per view.
static void bindDepthView(D3D12Encoder& encoder,
                          StageTextureSlot target,
                          D3D12TextureData& data,
                          D3D12_GPU_DESCRIPTOR_HANDLE view)
{
    auto* list = encoder.commands->list.get();
    const auto wanted = getRenderPassReadState(target.stage);

    if (!hasRenderPassReadState(data.depthState, wanted))
        transitionDepthForUse(list, data, wanted);

    list->SetGraphicsRootDescriptorTable(renderTextureParam(target.slot), view);
}

void bindStageTexture(D3D12Encoder* encoder,
                      StageTextureSlot target,
                      const Texture& texture)
{
    if (!canBindTextureSlot(encoder, target.slot))
        return;

    auto* data = static_cast<D3D12TextureData*>(texture.nativeTexture());

    if (data == nullptr || data->srv.gpu.ptr == 0)
        return;

    auto* list = encoder->commands->list.get();
    const auto wanted = getRenderPassReadState(target.stage);

    // A render target an earlier pass on this frame drew into is still in
    // RENDER_TARGET, and this is where it goes back. A plain texture is already
    // in the state this asks for and the helper records nothing.
    if (!hasRenderPassReadState(data->state, wanted))
        transitionTextureForUse(list, *data, wanted);

    // Only the SRV: the sampler is a static sampler in the root signature, chosen
    // by the register the shader's sampler was emitted at. See TextureSampling.
    list->SetGraphicsRootDescriptorTable(renderTextureParam(target.slot),
                                         data->srv.gpu);
}

void bindStageDepthTexture(D3D12Encoder* encoder,
                           StageTextureSlot target,
                           const Texture& renderTarget)
{
    if (!canBindTextureSlot(encoder, target.slot))
        return;

    auto* data = static_cast<D3D12TextureData*>(renderTarget.nativeTexture());

    if (!hasSampleableDepthView(data) || data->depthSrv.gpu.ptr == 0)
        return;

    bindDepthView(*encoder, target, *data, data->depthSrv.gpu);
}

void bindStageDepthSlice(D3D12Encoder* encoder,
                         StageTextureSlot target,
                         const Texture& renderTarget,
                         int slice)
{
    if (!canBindTextureSlot(encoder, target.slot))
        return;

    auto* data = static_cast<D3D12TextureData*>(renderTarget.nativeTexture());

    if (!hasSampleableDepthView(data))
        return;

    const auto view = data->depthSliceView(slice);

    if (view.ptr == 0)
        return;

    bindDepthView(*encoder, target, *data, view);
}

void bindPerBindSampler(D3D12Encoder* encoder, int slot, const Sampler& sampler)
{
    if (!canBindTextureSlot(encoder, slot))
        return;

    const auto descriptor = perBindSamplerDescriptor(sampler);

    if (descriptor.ptr == 0)
        return;

    encoder->commands->list->SetGraphicsRootDescriptorTable(renderSamplerParam(slot),
                                                            descriptor);
}

D3D12_GPU_VIRTUAL_ADDRESS storageBufferAddress(CommandContext& commands,
                                               const BufferRange& range)
{
    auto* data = getBufferDataAtOffset(range);

    if (data == nullptr)
        return 0;

    // The union of the two read states rather than one: the same buffer may be
    // bound to both stages, and asking for each in turn would barrier it back
    // and forth between two states that are both just "a shader is reading it".
    transitionForUse(commands, *data, renderPassAnyShaderRead);

    return getAddressAtOffset(*data, range.offset);
}

void bindVertexBufferRange(D3D12Encoder& encoder,
                           const BufferRange& range,
                           int index)
{
    auto* data = getBufferDataAtOffset(range);

    if (data == nullptr)
        return;

    auto& commands = *encoder.commands;
    transitionForUse(
        commands, *data, D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER);

    // A view starting part-way into the resource: the address moves up by the
    // offset and the size comes down by it, so the view still ends where the
    // buffer does and vertex zero is the byte at the offset.
    D3D12_VERTEX_BUFFER_VIEW view = {};
    view.BufferLocation = getAddressAtOffset(*data, range.offset);
    view.SizeInBytes = getSizeFromOffset(*data, range.offset);
    view.StrideInBytes = strideForSlot(encoder.strides, index);

    commands.list->IASetVertexBuffers(static_cast<UINT>(index), 1, &view);
}

void bindConstantBytes(D3D12Encoder& encoder,
                       UINT rootParameter,
                       const void* data,
                       int bytes)
{
    auto& commands = *encoder.commands;
    const auto byteCount = static_cast<std::size_t>(bytes);
    const auto address =
        commands.context->uploadConstants(commands, data, byteCount);

    if (address != 0)
        commands.list->SetGraphicsRootConstantBufferView(rootParameter, address);
}

static DXGI_FORMAT getIndexBufferFormat(IndexFormat format)
{
    if (format == IndexFormat::UInt16)
        return DXGI_FORMAT_R16_UINT;

    return DXGI_FORMAT_R32_UINT;
}

bool bindIndexRange(CommandContext& commands,
                    const BufferRange& indices,
                    IndexFormat format)
{
    auto* data = getBufferDataAtOffset(indices);

    if (data == nullptr)
        return false;

    transitionForUse(commands, *data, D3D12_RESOURCE_STATE_INDEX_BUFFER);

    D3D12_INDEX_BUFFER_VIEW view = {};
    view.BufferLocation = getAddressAtOffset(*data, indices.offset);
    view.SizeInBytes = getSizeFromOffset(*data, indices.offset);
    view.Format = getIndexBufferFormat(format);

    commands.list->IASetIndexBuffer(&view);

    return true;
}

static D3D12BufferData* getIndirectArguments(CommandContext& commands,
                                             const Buffer& buffer)
{
    auto* data = static_cast<D3D12BufferData*>(buffer.nativeBuffer());

    if (data == nullptr || data->resource == nullptr)
        return nullptr;

    transitionForUse(commands, *data, D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT);
    return data;
}

static D3D12BufferData* getIndirectCountArguments(CommandContext& commands,
                                                  IndirectDrawCount count)
{
    if (count.buffer == nullptr)
        return nullptr;

    return getIndirectArguments(commands, *count.buffer);
}

static ID3D12Resource* getIndirectCountResource(const D3D12BufferData* countData)
{
    if (countData == nullptr)
        return nullptr;

    return countData->resource.get();
}

struct IndirectDrawCall
{
    bool indexed = false;
    const IndirectDrawPlan& plan;
    const Buffer& arguments;
    IndirectDrawCount count;
};

static ID3D12CommandSignature* getIndirectSignature(CommandContext& commands,
                                                    const IndirectDrawCall& call)
{
    const auto stride = static_cast<UINT>(call.plan.stride);
    return commands.context->getDrawSignature(call.indexed, stride);
}

static bool isIndirectCountMissing(IndirectDrawCount count,
                                   const D3D12BufferData* countData)
{
    return count.buffer != nullptr && countData == nullptr;
}

static void recordExecuteIndirect(CommandContext& commands,
                                  const IndirectDrawCall& call)
{
    auto* signature = getIndirectSignature(commands, call);
    auto* data = getIndirectArguments(commands, call.arguments);
    auto* countData = getIndirectCountArguments(commands, call.count);

    if (signature == nullptr || data == nullptr)
        return;

    if (isIndirectCountMissing(call.count, countData))
        return;

    commands.list->ExecuteIndirect(signature,
                                   static_cast<UINT>(call.plan.drawCount),
                                   data->resource.get(),
                                   static_cast<UINT64>(call.plan.offset),
                                   getIndirectCountResource(countData),
                                   static_cast<UINT64>(call.count.offset));
}

void executeIndirectDraws(CommandContext& commands,
                          const IndirectDrawPlan& plan,
                          const Buffer& arguments,
                          IndirectDrawCount count)
{
    recordExecuteIndirect(commands, {false, plan, arguments, count});
}

void executeIndexedIndirectDraws(CommandContext& commands,
                                 const IndirectDrawPlan& plan,
                                 const Buffer& arguments,
                                 IndirectDrawCount count)
{
    recordExecuteIndirect(commands, {true, plan, arguments, count});
}
} // namespace eacp::GPU

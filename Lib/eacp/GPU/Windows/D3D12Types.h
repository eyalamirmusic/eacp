#pragma once

#include "D3D12Context.h"

#include "../Codegen/ShaderTypes.h"
#include "../Frame/ComputePass.h"
#include "../Frame/RenderPass.h"

// What the public GPU classes' opaque void* handles point to. Internal to the
// D3D12 backend; not part of GPU.h.

namespace eacp::GPU
{

// Slots map straight onto shader registers: uniform slot n = b<n>, input buffer
// slot n = t<n>, output buffer slot n = u<n>, texture slot n = t<n>/s<n>.
constexpr int maxUniformSlots = 2;
constexpr int maxBufferSlots = ComputePass::maxBufferSlots;
constexpr int maxTextureSlots = 4;

// Render root signature layout: root CBVs per stage, one single-descriptor
// table per texture slot, then per-stage root SRVs for storage buffers.
constexpr UINT renderVertexCBVParam(int slot)
{
    return static_cast<UINT>(slot);
}
constexpr UINT renderPixelCBVParam(int slot)
{
    return static_cast<UINT>(maxUniformSlots + slot);
}
constexpr UINT renderTextureParam(int slot)
{
    return static_cast<UINT>(2 * maxUniformSlots + slot);
}

// A root descriptor, not a table: a buffer SRV *is* a buffer view, so it binds
// by GPU address. Per stage, one HLSL global being visible to both functions.
constexpr UINT renderVertexSRVParam(int slot)
{
    return static_cast<UINT>(2 * maxUniformSlots + maxTextureSlots + slot);
}
constexpr UINT renderPixelSRVParam(int slot)
{
    return static_cast<UINT>(2 * maxUniformSlots + maxTextureSlots + maxBufferSlots
                             + slot);
}

// Textures hold the low t registers, so storage buffers start above them.
static_assert(maxTextureSlots <= RenderPass::bufferRegisterBase,
              "render buffer registers must start above every texture slot");

constexpr UINT renderBufferRegister(int slot)
{
    return static_cast<UINT>(RenderPass::bufferRegisterBase + slot);
}

// There is deliberately no renderSamplerParam: samplers are static samplers in
// the root signature, not descriptor tables. See TextureSampling.

// Compute root signature layout: root CBVs, root SRVs and UAVs for storage
// buffers, then a single-descriptor table per texture slot, read and write.
constexpr UINT computeCBVParam(int slot)
{
    return static_cast<UINT>(slot);
}
constexpr UINT computeSRVParam(int slot)
{
    return static_cast<UINT>(maxUniformSlots + slot);
}
constexpr UINT computeUAVParam(int slot)
{
    return static_cast<UINT>(maxUniformSlots + maxBufferSlots + slot);
}
constexpr UINT computeTextureSRVParam(int slot)
{
    return static_cast<UINT>(maxUniformSlots + 2 * maxBufferSlots + slot);
}
constexpr UINT computeTextureUAVParam(int slot)
{
    return static_cast<UINT>(maxUniformSlots + 2 * maxBufferSlots + maxTextureSlots
                             + slot);
}

// Textures share the t/u register spaces with storage buffers, so their
// registers start above every buffer slot's.
static_assert(maxBufferSlots <= ComputePass::textureRegisterBase,
              "compute texture registers must start above every buffer slot");

constexpr UINT computeTextureRegister(int slot)
{
    return static_cast<UINT>(ComputePass::textureRegisterBase + slot);
}

// What ShaderLibrary::nativeLibrary() points to: D3D12 consumes raw bytecode at
// pipeline creation, so there are no shader objects.
struct D3D12ShaderProgram
{
    winrt::com_ptr<ID3DBlob> vertexBytecode;
    winrt::com_ptr<ID3DBlob> pixelBytecode;
    winrt::com_ptr<ID3DBlob> computeBytecode;
};

// What RenderPipeline::nativeState() points to: the PSO plus the draw-time
// state D3D12 keeps outside it.
struct D3D12Pipeline
{
    winrt::com_ptr<ID3D12PipelineState> state;
    D3D12_PRIMITIVE_TOPOLOGY topology = D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;

    // One entry per bound slot; a single-buffer pipeline carries one at index 0.
    Vector<UINT> strides;
    bool depth = false;
};

// Falls back to slot 0's stride, so a single-buffer pipeline still binds when
// the caller passes a non-zero slot index.
inline UINT strideForSlot(const Vector<UINT>& strides, int slot)
{
    if (slot >= 0 && slot < strides.size())
        return strides[slot];
    if (!strides.empty())
        return strides[0];
    return 0;
}

// What Buffer::nativeBuffer() points to. State is tracked per recording:
// buffers decay to COMMON at every ExecuteCommandLists and are promoted on
// first use, so only two states within one recording need a barrier.
struct D3D12BufferData
{
    winrt::com_ptr<ID3D12Resource> resource;
    UINT64 size = 0;
    D3D12_RESOURCE_STATES state = D3D12_RESOURCE_STATE_COMMON;
    std::uint64_t recordingId = 0;
};

// What Texture::nativeTexture()/nativeReadView() point to. The SRV slot lives
// in the context's shader-visible heap for the texture's whole lifetime. There
// is no sampler slot: every sampler is static. See TextureSampling.
struct D3D12TextureData
{
    winrt::com_ptr<ID3D12Resource> resource;
    DescriptorSlot srv;

    // From the same CBV_SRV_UAV heap as the SRV. Root descriptors are buffers
    // only, so a texture UAV binds through the compute descriptor table.
    DescriptorSlot uav;

    // RTV descriptors are not shader-visible, so a render target owns its own
    // one-descriptor heap rather than taking a slot from a shared one.
    winrt::com_ptr<ID3D12DescriptorHeap> rtvHeap;
    D3D12_CPU_DESCRIPTOR_HANDLE rtv = {};

    // Likewise for the DSV. The depth resource rests in DEPTH_WRITE for its
    // whole lifetime, so it needs no state tracking and no barriers.
    winrt::com_ptr<ID3D12Resource> depthResource;
    winrt::com_ptr<ID3D12DescriptorHeap> dsvHeap;
    D3D12_CPU_DESCRIPTOR_HANDLE dsv = {};

    // Tracked for the resource's lifetime, not per recording: unlike a buffer a
    // texture does not decay to COMMON at ExecuteCommandLists.
    D3D12_RESOURCE_STATES state = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;

    bool isRenderTarget() const { return rtv.ptr != 0; }
    bool isComputeWritable() const { return uav.cpu.ptr != 0; }
    bool hasDepth() const { return dsv.ptr != 0; }
};

// The frame's colour target, owned by GPUView and valid for the Frame's
// lifetime. The back buffer arrives in PRESENT and must be returned to it
// before the frame's submit.
struct D3D12Drawable
{
    IDXGISwapChain3* swapChain = nullptr;
    ID3D12Resource* backBuffer = nullptr;
    D3D12_CPU_DESCRIPTOR_HANDLE backBufferView = {};
    UINT width = 0;
    UINT height = 0;
};

// Owned by GPUView, kept in RENDER_TARGET between frames. When present the pass
// renders into it and the frame resolves it into the back buffer.
struct D3D12MsaaTarget
{
    ID3D12Resource* texture = nullptr;
    D3D12_CPU_DESCRIPTOR_HANDLE view = {};
    DXGI_FORMAT format = DXGI_FORMAT_B8G8R8A8_UNORM;
};

// Owned by GPUView, kept in DEPTH_WRITE for life so it never needs barriers.
struct D3D12DepthTarget
{
    D3D12_CPU_DESCRIPTOR_HANDLE view = {};
};

// Carries beginPass's recording to the RenderPass. The CommandContext stays
// owned by the Frame; the encoder is owned by the pass.
struct D3D12Encoder
{
    CommandContext* commands = nullptr;
    Vector<UINT> strides;

    // Where a timed pass writes its closing timestamp, beginPass having
    // recorded the opening one. Null and -1 when the pass is not timed.
    ID3D12QueryHeap* queryHeap = nullptr;
    int endQuery = -1;
};

// The compute sibling, its CommandContext owned by the CommandBuffer. A pass on
// a CommandBuffer is never timed, there being no frame to attribute it to.
struct D3D12ComputeEncoder
{
    CommandContext* commands = nullptr;

    ID3D12QueryHeap* queryHeap = nullptr;
    int endQuery = -1;
};

template <typename Encoder>
inline void endTimedPass(const Encoder& encoder)
{
    if (encoder.queryHeap == nullptr || encoder.endQuery < 0
        || encoder.commands == nullptr)
        return;

    encoder.commands->list->EndQuery(encoder.queryHeap,
                                     D3D12_QUERY_TYPE_TIMESTAMP,
                                     static_cast<UINT>(encoder.endQuery));
}

// First use in a recording is free: the buffer decayed to COMMON at the last
// execute, and promotion covers any first state.
inline void transitionForUse(CommandContext& commands,
                             D3D12BufferData& buffer,
                             D3D12_RESOURCE_STATES target)
{
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

// No first-use-is-free case: a texture does not decay to COMMON on execute, so
// it stays in whatever the previous frame's last pass left it in.
inline void transitionTextureForUse(ID3D12GraphicsCommandList* list,
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

// The state every compute pass needs before it can dispatch. Binds the same two
// heaps the render path does, so the bound set does not depend on which pass ran
// last, even though the compute signature declares no sampler table.
inline void bindComputeRootState(D3D12Context& context,
                                 ID3D12GraphicsCommandList* list)
{
    ID3D12DescriptorHeap* heaps[] = {context.getTextureHeap(),
                                     context.getSamplerHeap()};
    list->SetDescriptorHeaps(2, heaps);
    list->SetComputeRootSignature(context.getComputeRootSignature());

    // Resource Binding Tier 1 hardware silently drops a dispatch if any
    // descriptor table the root signature declares is unset, even ones the
    // kernel never touches, so seed every table with a null descriptor.
    const auto nullSrv = context.getNullTextureDescriptor();
    const auto nullUav = context.getNullTextureUAVDescriptor();

    if (nullSrv.ptr == 0 || nullUav.ptr == 0)
        return;

    for (auto slot = 0; slot < maxTextureSlots; ++slot)
    {
        list->SetComputeRootDescriptorTable(computeTextureSRVParam(slot), nullSrv);
        list->SetComputeRootDescriptorTable(computeTextureUAVParam(slot), nullUav);
    }
}

// A plain transition barrier for resources with externally known states (back
// buffers, MSAA targets).
inline void transition(ID3D12GraphicsCommandList* list,
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
} // namespace eacp::GPU

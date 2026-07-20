#pragma once

#include "D3D12Context.h"

#include "../Codegen/ShaderTypes.h"

// Internal shared types for the Windows/D3D12 GPU backend. The public GPU
// classes expose opaque void* handles (nativeBuffer/nativeLibrary/nativeState/
// ...); these structs are what those handles point to, so the separate
// translation units (Shader, Pipeline, Frame, RenderPass, View) agree on the
// concrete layout without leaking D3D types into the public headers. Not part
// of GPU.h.

namespace eacp::GPU
{

// The binding model both root signatures implement. Slots map straight onto
// shader registers: uniform slot n = b<n>, input buffer slot n = t<n>, output
// buffer slot n = u<n>, texture slot n = t<n>/s<n> — the same registers the
// shader emitter and the hand-written HLSL in tests and examples declare.
constexpr int maxUniformSlots = 2;
constexpr int maxBufferSlots = 4;
constexpr int maxTextureSlots = 4;

// Render root signature parameter layout: root CBVs per stage, then one
// single-descriptor table per texture slot (SRV, then sampler — tables cannot
// mix heap types). Single-descriptor tables let each texture bind its
// persistent heap slot directly, with no per-frame descriptor copying.
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

// There is deliberately no renderSamplerParam: samplers are static samplers in
// the root signature, not descriptor tables. See TextureSampling.

// Compute root signature parameter layout: root CBVs, then root SRVs and root
// UAVs. Root descriptors bind structured buffers by GPU address, so compute
// needs no descriptor heap at all.
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

// Result of compiling a ShaderSource. D3D12 consumes raw bytecode at pipeline
// creation, so the library stores blobs rather than shader objects. Pointed to
// by ShaderLibrary::nativeLibrary().
struct D3D12ShaderProgram
{
    winrt::com_ptr<ID3DBlob> vertexBytecode;
    winrt::com_ptr<ID3DBlob> pixelBytecode;
    winrt::com_ptr<ID3DBlob> computeBytecode;
};

// A compiled pipeline plus the draw-time state D3D12 keeps outside the PSO.
// Pointed to by RenderPipeline::nativeState().
struct D3D12Pipeline
{
    winrt::com_ptr<ID3D12PipelineState> state;
    D3D12_PRIMITIVE_TOPOLOGY topology = D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
    // Per-slot strides so multi-buffer draws (e.g. instancing) know the
    // stride at each bound slot when setVertexBuffer wires the D3D12 view.
    // Legacy single-buffer pipelines carry one entry at index 0.
    Vector<UINT> strides;
    bool depth = false;
};

// The single "stride for a bound slot" rule shared by RenderPipeline (which
// builds the table) and RenderPass::setVertexBuffer (which reads it): if the
// slot has an explicit stride use it; otherwise fall back to slot 0's stride
// so legacy single-buffer pipelines (which build a one-entry table for
// slot 0) still bind correctly when the caller happens to pass a non-zero
// slot index. Kept in one place so the two sites can't drift apart on the
// platform I can't test.
inline UINT strideForSlot(const Vector<UINT>& strides, int slot)
{
    if (slot >= 0 && slot < strides.size())
        return strides[slot];
    if (!strides.empty())
        return strides[0];
    return 0;
}

// What Buffer::nativeBuffer() points to. Tracks the resource's state within
// the current recording: buffers decay to COMMON after every
// ExecuteCommandLists and are implicitly promoted on first use, so a barrier
// is only needed when one recording uses the same buffer in two states.
struct D3D12BufferData
{
    winrt::com_ptr<ID3D12Resource> resource;
    UINT64 size = 0;
    D3D12_RESOURCE_STATES state = D3D12_RESOURCE_STATE_COMMON;
    std::uint64_t recordingId = 0;
};

// What Texture::nativeTexture()/nativeReadView() point to. The SRV slot lives
// in the context's shader-visible heap for the texture's whole lifetime;
// binding is just a root-table pointer update. There is no sampler slot: every
// sampler is static in the root signature. See TextureSampling.
struct D3D12TextureData
{
    winrt::com_ptr<ID3D12Resource> resource;
    DescriptorSlot srv;
};

// The frame's color target. All members are owned by GPUView and stay valid
// for the lifetime of the Frame. Pointed to by the drawable handle passed to
// Frame. The back buffer is in PRESENT state on entry and must be returned to
// it before the frame's submit.
struct D3D12Drawable
{
    IDXGISwapChain3* swapChain = nullptr;
    ID3D12Resource* backBuffer = nullptr;
    D3D12_CPU_DESCRIPTOR_HANDLE backBufferView = {};
    UINT width = 0;
    UINT height = 0;
};

// Optional multisample target, kept in RENDER_TARGET state between frames.
// When present the pass renders into it and the frame resolves it into the
// swapchain back buffer. Owned by GPUView.
struct D3D12MsaaTarget
{
    ID3D12Resource* texture = nullptr;
    D3D12_CPU_DESCRIPTOR_HANDLE view = {};
    DXGI_FORMAT format = DXGI_FORMAT_B8G8R8A8_UNORM;
};

// Optional depth buffer, kept in DEPTH_WRITE state for its whole lifetime so
// it never needs barriers. Owned by GPUView.
struct D3D12DepthTarget
{
    D3D12_CPU_DESCRIPTOR_HANDLE view = {};
};

// Carries the frame's recording (and the active pipeline's per-slot strides)
// from beginPass to the RenderPass. The CommandContext stays owned by the
// Frame, which submits and presents on destruction; the encoder is owned by
// the pass. Strides are per-slot so multi-buffer draws (e.g. instancing)
// bind each slot with its own stride.
struct D3D12Encoder
{
    CommandContext* commands = nullptr;
    Vector<UINT> strides;
};

// The compute sibling of D3D12Encoder. The CommandContext stays owned by the
// CommandBuffer, which submits on commit().
struct D3D12ComputeEncoder
{
    CommandContext* commands = nullptr;
};

// Records the barrier a buffer needs before being used in the target state.
// First use in a recording is free: the buffer was in COMMON (they decay
// there after every execute) and promotion covers any first state.
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

#pragma once

#include "D3D12Context.h"

#include "../Codegen/ShaderTypes.h"
#include "../Frame/ComputePass.h"
#include "../Frame/RenderPass.h"

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
// mix heap types), then per-stage root SRVs for the storage buffers a shader
// subscripts. Single-descriptor tables let each texture bind its persistent
// heap slot directly, with no per-frame descriptor copying.
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

// A storage buffer read by a shader stage is a root descriptor, not a table:
// unlike a texture, a buffer SRV *is* a buffer view, so it binds by GPU address
// and needs no heap. Per stage, like the CBVs and for the same reason - one
// HLSL global is visible to both functions, so each stage names its own root
// parameter at the same register.
constexpr UINT renderVertexSRVParam(int slot)
{
    return static_cast<UINT>(2 * maxUniformSlots + maxTextureSlots + slot);
}
constexpr UINT renderPixelSRVParam(int slot)
{
    return static_cast<UINT>(2 * maxUniformSlots + maxTextureSlots + maxBufferSlots
                             + slot);
}

// The render signature's mirror of computeTextureRegister: a shader's textures
// hold the low t registers, so its storage buffers start above every texture
// slot. The emitter writes these from RenderPass::bufferRegisterBase.
static_assert(maxTextureSlots <= RenderPass::bufferRegisterBase,
              "render buffer registers must start above every texture slot");

constexpr UINT renderBufferRegister(int slot)
{
    return static_cast<UINT>(RenderPass::bufferRegisterBase + slot);
}

// There is deliberately no renderSamplerParam: samplers are static samplers in
// the root signature, not descriptor tables. See TextureSampling.

// Compute root signature parameter layout: root CBVs, then root SRVs and root
// UAVs for the storage buffers, then one single-descriptor table per texture
// slot for reads and another for writes.
//
// Buffers are root descriptors, which bind by GPU address and need no heap at
// all. Textures cannot be: a root descriptor is a buffer view and nothing else,
// so a texture - read or written - goes through a table, which is why the
// compute path binds descriptor heaps the way the render path does.
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

// A kernel's textures share the t/u register spaces with its storage buffers,
// and the two slot spaces are counted separately, so a texture's registers
// start above every buffer slot's. The emitter writes the same registers from
// ComputePass::textureRegisterBase, which this holds it to.
static_assert(maxBufferSlots <= ComputePass::textureRegisterBase,
              "compute texture registers must start above every buffer slot");

constexpr UINT computeTextureRegister(int slot)
{
    return static_cast<UINT>(ComputePass::textureRegisterBase + slot);
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

    // A compute-writable texture also owns a UAV, from the same heap the SRV
    // came from (the allocator is CBV_SRV_UAV). A texture UAV cannot be a root
    // descriptor the way a buffer UAV is - root descriptors are buffers only -
    // so this is what the compute descriptor table points at.
    DescriptorSlot uav;

    // A render-target texture also owns one RTV, in a heap of its own: RTV
    // descriptors are not shader-visible, so there is no shared heap to take one
    // from the way the SRV does, and one descriptor per target is cheap.
    winrt::com_ptr<ID3D12DescriptorHeap> rtvHeap;
    D3D12_CPU_DESCRIPTOR_HANDLE rtv = {};

    // A target created with TextureDescriptor::depth owns its depth buffer and
    // that buffer's DSV, on the same terms as the RTV above and for the same
    // reason: DSV descriptors are not shader-visible either. The resource rests
    // in DEPTH_WRITE for its whole lifetime - nothing else ever touches it - so
    // unlike the colour resource it needs no state tracking and no barriers,
    // which is what D3D12DepthTarget below already relies on for the drawable.
    winrt::com_ptr<ID3D12Resource> depthResource;
    winrt::com_ptr<ID3D12DescriptorHeap> dsvHeap;
    D3D12_CPU_DESCRIPTOR_HANDLE dsv = {};

    // A plain texture rests in PIXEL_SHADER_RESOURCE forever - it is only ever
    // sampled - and a render target moves between that and RENDER_TARGET as the
    // passes go by. Unlike a buffer this does not decay to COMMON at
    // ExecuteCommandLists, so the state is tracked for the resource's lifetime
    // rather than per recording, and both sites that use one go through the
    // helper below rather than reasoning about it locally.
    D3D12_RESOURCE_STATES state = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;

    bool isRenderTarget() const { return rtv.ptr != 0; }
    bool isComputeWritable() const { return uav.cpu.ptr != 0; }
    bool hasDepth() const { return dsv.ptr != 0; }
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

// The texture sibling of transitionForUse: records the barrier a texture needs
// before being used in the target state, and remembers what it is now in.
//
// There is no first-use-is-free case here, because a texture does not decay to
// COMMON when a recording executes the way a buffer does - what it was left in
// by the previous frame's last pass is what it is still in. Getting that wrong
// is a barrier the debug layer rejects rather than anything visible, which is
// exactly why the tracking lives here and not at the two call sites.
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

// The state every compute pass needs bound before it can dispatch. Shared by
// Frame::beginCompute and CommandBuffer::beginCompute so the two cannot drift
// apart on the platform I cannot test - the compute sibling of
// Frame::Native::bindRootState.
//
// The heaps are here because a kernel's textures bind through descriptor
// tables: buffers are root descriptors and need no heap, which is why the
// compute path bound none until textures arrived. The same two the render path
// binds, so whichever pass ran last leaves the same set behind - the compute
// signature declares no sampler table (every sampler in it is a static sampler,
// see TextureSampling), but binding one heap here and two there would make the
// bound heaps depend on the order the passes happened to run in.
inline void bindComputeRootState(D3D12Context& context,
                                 ID3D12GraphicsCommandList* list)
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

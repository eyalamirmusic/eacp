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
// buffer slot n = u<n>, texture slot n = t<n> — the same registers the shader
// emitter and the hand-written HLSL in tests and examples declare. A texture's
// *sampler* is the one binding that does not follow its slot: there are
// samplingConfigurations static samplers at s0.. and every texture shares the
// one its declared sampling picks, so a slot count costs no sampler registers.
// See TextureSampling.
constexpr int maxUniformSlots = 2;
constexpr int maxBufferSlots = ComputePass::maxBufferSlots;

// Eight, and the number is a shader model limit rather than a hardware one: the
// slots are single-descriptor tables, which cost one root DWORD each, so this
// is nearly free. It was four until a port needed five in one program - Doom 3
// lights a surface from a bump map, a falloff, a light projection, a diffuse
// map and a specular map, none of which fold into another.
constexpr int maxTextureSlots = 8;

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
    bool stencil = false;
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

    // Set for a BufferStorage::Streaming buffer, whose resource is on the
    // UPLOAD heap. There is no state tracking to do for one: an upload-heap
    // resource is created in GENERIC_READ and D3D12 forbids transitioning it
    // out, which is the whole reason a streamed write records nothing. The
    // flag is what stops transitionForUse from trying - a barrier off
    // GENERIC_READ is a debug-layer error, and on a real driver it is a
    // command list that will not close.
    bool uploadHeap = false;
};

// What Texture::nativeTexture()/nativeReadView() point to. The SRV slot lives
// in the context's shader-visible heap for the texture's whole lifetime;
// binding is just a root-table pointer update. There is no sampler slot: every
// sampler is static in the root signature. See TextureSampling.
// The one format a depth attachment is created, cleared, viewed and compiled
// against. Kept in one place because those four have to name the same value:
// a resource, its optimised clear value, its DSV and the pipeline's DSVFormat
// disagreeing is a validation error at the draw rather than at creation, which
// is the slowest possible place to find it.
//
// D32_FLOAT_S8X24_UINT rather than D24_UNORM_S8_UINT so the depth plane keeps
// the same 32-bit float precision, and the same near/far behaviour, whether or
// not the stencil plane is there - and so it matches Metal, whose Apple-silicon
// devices do not have the 24-bit combined format at all.
inline DXGI_FORMAT depthAttachmentFormat(bool withStencil)
{
    return withStencil ? DXGI_FORMAT_D32_FLOAT_S8X24_UINT : DXGI_FORMAT_D32_FLOAT;
}

// What the depth *resource* is created as, which is the same thing as the
// attachment format unless a shader is also going to read it. D3D12 will not
// give one resource two views of two different fully-typed formats, so a
// sampleable depth buffer is created typeless and the DSV and the SRV each name
// the type they want out of it. Metal needs none of this: a depth texture there
// keeps its format and gains a usage bit.
inline DXGI_FORMAT depthResourceFormat(bool withStencil, bool sampleable)
{
    if (!sampleable)
        return depthAttachmentFormat(withStencil);

    return withStencil ? DXGI_FORMAT_R32G8X24_TYPELESS : DXGI_FORMAT_R32_TYPELESS;
}

// The read half of that pair: one float channel, which is what
// ShaderBuilder::depthTexture declares and what Metal's depth2d<float> hands
// back from the same sample. With a stencil plane the depth still reads as the
// first 32 bits and the other 32 are named as the padding they are.
inline DXGI_FORMAT depthShaderResourceFormat(bool withStencil)
{
    return withStencil ? DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS
                       : DXGI_FORMAT_R32_FLOAT;
}

// The format a depth resolve names, which is neither of the two above: a
// resolve is a copy, so it wants the plane as data rather than as a depth
// attachment, and the resolve modes MIN and MAX are only defined over a
// single-component typed format. The stencil half of a combined buffer is not
// resolved and is named as the padding it is here.
inline DXGI_FORMAT depthResolveFormat(bool withStencil)
{
    return withStencil ? DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS
                       : DXGI_FORMAT_R32_FLOAT;
}

// Which planes a pass clears. Both when the buffer has both, which is what
// keeps a stencilled pass starting from a value it named rather than from
// whatever the last frame left in the plane.
inline D3D12_CLEAR_FLAGS depthClearFlags(bool withStencil)
{
    return withStencil ? static_cast<D3D12_CLEAR_FLAGS>(D3D12_CLEAR_FLAG_DEPTH
                                                        | D3D12_CLEAR_FLAG_STENCIL)
                       : D3D12_CLEAR_FLAG_DEPTH;
}

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

    // A multisampled target owns a second colour resource and a second RTV: the
    // one a pass actually renders into, resolved into `resource` at the end of
    // every pass. `resource` is never a render target on that path - it is the
    // resolve destination - which is why the two carry their own states.
    //
    // The multisample resource rests in RENDER_TARGET, as the drawable's own
    // MSAA target does, and steps out to RESOLVE_SOURCE for the resolve and back.
    winrt::com_ptr<ID3D12Resource> msaaResource;
    winrt::com_ptr<ID3D12DescriptorHeap> msaaRtvHeap;
    D3D12_CPU_DESCRIPTOR_HANDLE msaaRtv = {};
    D3D12_RESOURCE_STATES msaaState = D3D12_RESOURCE_STATE_RENDER_TARGET;

    // The colour format both resources were created in. ResolveSubresource has
    // to be told it and a resource will not hand it back without a GetDesc.
    DXGI_FORMAT colorFormat = DXGI_FORMAT_R8G8B8A8_UNORM;

    // How many samples a pass into this target takes: 1 unless msaaResource is
    // there, and then the count both it and the depth buffer were created at.
    int sampleCount = 1;

    // A target created with TextureDescriptor::depth owns its depth buffer and
    // that buffer's DSV, on the same terms as the RTV above and for the same
    // reason: DSV descriptors are not shader-visible either. The resource rests
    // in DEPTH_WRITE for its whole lifetime - nothing else ever touches it - so
    // unlike the colour resource it needs no state tracking and no barriers,
    // which is what D3D12DepthTarget below already relies on for the drawable.
    winrt::com_ptr<ID3D12Resource> depthResource;
    winrt::com_ptr<ID3D12DescriptorHeap> dsvHeap;
    D3D12_CPU_DESCRIPTOR_HANDLE dsv = {};

    // Whether that buffer was created in the combined format, which is what
    // decides both the clear flags the pass uses and whether a pipeline drawing
    // here must set RenderPipelineDescriptor::stencil.
    bool depthHasStencil = false;

    // The read view of that same depth resource, and the state it is currently
    // in - both of which a target created without
    // TextureDescriptor::sampleableDepth simply does not have. The paragraph
    // above says the depth resource rests in DEPTH_WRITE and needs no tracking,
    // and that stays true of every target but this one: a sampled depth buffer
    // moves to PIXEL_SHADER_RESOURCE for the pass that reads it and back to
    // DEPTH_WRITE for the pass that attaches it, which is the pair of barriers
    // sampleableDepth's documentation charges it for.
    DescriptorSlot depthSrv;
    D3D12_RESOURCE_STATES depthState = D3D12_RESOURCE_STATE_DEPTH_WRITE;

    // The single-sampled buffer a multisampled target's depth plane resolves
    // into, and the resource depthSrv actually views on that path. Null
    // everywhere else, where the attachment is what a shader reads.
    //
    // It exists because a shader eacp generated declares a Texture2D, not a
    // Texture2DMS with a sample index to choose between - see
    // TextureDescriptor::sampleCount. The resolve is a ResolveSubresourceRegion
    // in RESOLVE_MODE_MAX, D3D12 having no equivalent of Metal's sample-0 depth
    // filter, so the value that comes out is the furthest of the samples rather
    // than the first; on a silhouette pixel those differ, and everywhere else
    // they do not.
    //
    // depthState tracks whichever of the two depthSrv views, since that is the
    // one a bind moves.
    winrt::com_ptr<ID3D12Resource> resolvedDepthResource;
    D3D12_RESOURCE_STATES msaaDepthState = D3D12_RESOURCE_STATE_DEPTH_WRITE;

    // What the shader route to that resolve draws with, where the driver has
    // no resolve of its own (DriverQuirks::noDepthResolve): a read view of the
    // multisampled attachment for the pixel shader to load samples from, and a
    // DSV on the resolved buffer for it to write SV_Depth through. Empty
    // everywhere else. See resolveDepthWithShader.
    DescriptorSlot msaaDepthSrv;
    winrt::com_ptr<ID3D12DescriptorHeap> resolvedDsvHeap;
    D3D12_CPU_DESCRIPTOR_HANDLE resolvedDsv = {};

    ID3D12Resource* sampledDepthResource() const
    {
        return resolvedDepthResource != nullptr ? resolvedDepthResource.get()
                                                : depthResource.get();
    }

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
    bool hasStencil() const { return hasDepth() && depthHasStencil; }

    bool hasSampleableDepth() const { return hasDepth() && depthSrv.cpu.ptr != 0; }

    bool isMultisampled() const
    {
        return msaaResource != nullptr && msaaRtv.ptr != 0;
    }

    // Where a pass into this target draws, and where it clears: the multisample
    // resource when there is one, and the texture itself otherwise.
    D3D12_CPU_DESCRIPTOR_HANDLE attachmentView() const
    {
        return isMultisampled() ? msaaRtv : rtv;
    }
};

// The depth resource moved between the two states it can be in. The colour
// resource's helper is transitionTextureForUse below; this is its depth twin,
// separate because the two planes are two resources with two states and one
// D3D12TextureData.
// On a multisampled target the resource this moves is the *resolved* buffer -
// the one depthSrv views and the one a shader reads - and the attachment stays
// in DEPTH_WRITE throughout, which is what lets the pass go on writing depth
// while an earlier pass's resolve of it is being sampled.
inline void transitionDepthForUse(ID3D12GraphicsCommandList* list,
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

    // Whether the buffer behind that view carries a stencil plane, so the pass
    // knows whether to clear one. A DSV says nothing about its own format.
    bool stencil = false;
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

    // Where a timed pass writes its closing timestamp. The opening one is
    // recorded by beginPass; this one has to wait for the pass to end, which is
    // the pass's own business and not the frame's. Null and -1 when the pass
    // carries no label and is therefore not timed.
    ID3D12QueryHeap* queryHeap = nullptr;
    int endQuery = -1;

    // The multisampled target this pass is drawing into, or null. Held so the
    // resolve happens when the pass *ends* rather than when the frame does: what
    // the target holds has to be right for whatever reads it next, and a read
    // between two passes is the case the whole thing exists for. See
    // TextureDescriptor::sampleCount, and resolveMultisampledTarget below.
    D3D12TextureData* resolveTarget = nullptr;
};

// The compute sibling of D3D12Encoder. The CommandContext stays owned by the
// CommandBuffer, which submits on commit().
struct D3D12ComputeEncoder
{
    CommandContext* commands = nullptr;

    // See D3D12Encoder. A compute pass on a Frame can be timed the same way; one
    // on a CommandBuffer cannot, there being no frame to attribute it to.
    ID3D12QueryHeap* queryHeap = nullptr;
    int endQuery = -1;
};

// Closes a timed pass, wherever the pass happens to end. Both encoders carry
// the same two fields for it, so both end the same way.
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

// Records the barrier a buffer needs before being used in the target state.
// First use in a recording is free: the buffer was in COMMON (they decay
// there after every execute) and promotion covers any first state.
inline void transitionForUse(CommandContext& commands,
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

// What a pass into a multisampled target does on its way out: flatten the
// samples into the target's own resource, and into the resolved depth buffer
// where the target has one, leaving both attachments untouched so the next pass
// can load them.
//
// **Recorded at the end of every pass, not at the end of the frame**, which is
// where the drawable's resolve lives and is the one thing that could not be
// copied from it. A texture target is readable between passes - sampled by the
// next pass, blitted out, read back to the CPU - and none of those know which
// pass was the last, so the resolved picture has to be current whenever a pass
// has just ended. See TextureDescriptor::sampleCount.
//
// The colour resolve leaves the target in RESOLVE_DEST rather than putting it
// back: `state` tracks it, so whatever comes next - a sample, a read, another
// pass - emits the barrier it needs from there and this pays for none it does
// not.
//
// Depth is a ResolveSubresourceRegion in RESOLVE_MODE_MAX, ResolveSubresource
// having no depth form: MIN and MAX are the only modes D3D12 offers a
// depth-stencil format, so the resolved value is the furthest sample rather than
// Metal's first one. It runs on plane 0, the depth plane, the stencil plane
// having no resolve and nothing that reads it. On a driver that refuses the
// call - DriverQuirks::noDepthResolve - the same value is drawn instead; see
// resolveDepthWithShader.
void resolveDepthWithShader(CommandContext& commands, D3D12TextureData& texture);

inline void resolveMultisampledTarget(CommandContext& commands,
                                      D3D12TextureData& texture)
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

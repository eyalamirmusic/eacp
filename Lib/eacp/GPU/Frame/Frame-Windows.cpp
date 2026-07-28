#include <eacp/Core/Utils/WinInclude.h>

#include "Frame.h"

#include "../Device/Device.h"
#include "../Windows/D3D12Types.h"

// Windows/D3D12 backend. The drawable and msaaTexture handles point at the
// D3D12Drawable / D3D12MsaaTarget owned by GPUView. The frame owns one
// CommandContext recording: beginPass transitions and clears the target, the
// pass records draws onto it, and the destructor resolves any multisample
// target, returns the back buffer to PRESENT, executes the recording and
// presents the swapchain (mirroring the Metal frame's present-on-destroy).

namespace eacp::GPU
{
struct Frame::Native
{
    Native(Device& device,
           void* drawableHandle,
           void* msaaTextureHandle,
           void* depthTextureHandle)
        : drawable(static_cast<D3D12Drawable*>(drawableHandle))
        , msaa(static_cast<D3D12MsaaTarget*>(msaaTextureHandle))
        , depth(static_cast<D3D12DepthTarget*>(depthTextureHandle))
    {
        if (device.isValid() && drawable != nullptr)
            commands = getD3D12Context().acquire();
    }

    // Off-screen snapshot target (GPUView::renderNativeContent). The colour
    // target is passed as a D3D12Drawable with a null swapChain, so beginPass
    // renders into it exactly like a back buffer but the destructor resolves and
    // hands it back for read-back instead of presenting.
    Native(Device& device, const OffscreenTarget& target)
        : drawable(static_cast<D3D12Drawable*>(target.colorTexture))
        , msaa(static_cast<D3D12MsaaTarget*>(target.msaaTexture))
        , depth(static_cast<D3D12DepthTarget*>(target.depthTexture))
        , offscreen(true)
    {
        if (device.isValid() && drawable != nullptr
            && drawable->backBuffer != nullptr)
            commands = getD3D12Context().acquire();
    }

    bool useMsaa() const { return msaa != nullptr && msaa->texture != nullptr; }

    // The state every render pass needs bound before it can draw, whether it
    // targets the back buffer or an app-owned texture. Shared so the two paths
    // cannot drift apart on the platform I cannot test.
    void bindRootState(D3D12Context& context, ID3D12GraphicsCommandList* list)
    {
        // The root signature and heaps are fixed for every render pipeline, so
        // binding them here frees the pass from caring about call ordering.
        ID3D12DescriptorHeap* heaps[] = {context.getTextureHeap(),
                                         context.getSamplerHeap()};
        list->SetDescriptorHeaps(2, heaps);
        list->SetGraphicsRootSignature(context.getRenderRootSignature());

        // Resource Binding Tier 1 hardware requires *every* descriptor table the
        // root signature declares to be populated before a draw, even the ones
        // the shader never reads — an unset table drops the draw entirely rather
        // than failing loudly. The signature is shared and declares
        // maxTextureSlots of them, while a typical shader binds one, so the rest
        // are seeded with the null descriptor here; setFragmentTexture
        // overwrites the slots that carry a real texture.
        //
        // Tier 2+ hardware ignores unset tables, which is why this only ever
        // showed up on an Arm laptop: no text drew, and nothing was logged
        // without the D3D12 validation layer installed.
        //
        // Only the SRV tables need this. The root signature declares no sampler
        // tables at all any more - samplers are static samplers baked into it,
        // picked by the register the shader emitted its sampler at. See
        // TextureSampling.
        const auto nullTexture = context.getNullTextureDescriptor();

        if (nullTexture.ptr == 0)
            return;

        for (auto slot = 0; slot < maxTextureSlots; ++slot)
            list->SetGraphicsRootDescriptorTable(renderTextureParam(slot),
                                                 nullTexture);
    }

    CommandContext* commands = nullptr;
    D3D12Drawable* drawable = nullptr;
    D3D12MsaaTarget* msaa = nullptr;
    D3D12DepthTarget* depth = nullptr;
    bool passBegun = false;
    bool offscreen = false;
};

Frame::Frame(Device& device, void* drawable, void* msaaTexture, void* depthTexture)
    : impl(device, drawable, msaaTexture, depthTexture)
{
}

Frame::Frame(Device& device, const OffscreenTarget& target)
    : impl(device, target)
{
}

Frame::~Frame()
{
    if (impl->commands == nullptr || impl->drawable == nullptr)
        return;

    auto* list = impl->commands->list.get();
    auto* backBuffer = impl->drawable->backBuffer;

    if (impl->offscreen)
    {
        // Off-screen snapshot: resolve any MSAA into the colour texture, leave it
        // in COPY_SOURCE for GPUView's read-back, then run the GPU to completion
        // (no swapchain to present). The colour texture was created in
        // RESOLVE_DEST when multisampling and RENDER_TARGET otherwise.
        if (impl->useMsaa() && backBuffer != nullptr)
        {
            transition(list,
                       impl->msaa->texture,
                       D3D12_RESOURCE_STATE_RENDER_TARGET,
                       D3D12_RESOURCE_STATE_RESOLVE_SOURCE);

            list->ResolveSubresource(
                backBuffer, 0, impl->msaa->texture, 0, impl->msaa->format);

            transition(list,
                       backBuffer,
                       D3D12_RESOURCE_STATE_RESOLVE_DEST,
                       D3D12_RESOURCE_STATE_COPY_SOURCE);
        }
        else if (backBuffer != nullptr)
        {
            transition(list,
                       backBuffer,
                       D3D12_RESOURCE_STATE_RENDER_TARGET,
                       D3D12_RESOURCE_STATE_COPY_SOURCE);
        }

        auto& context = getD3D12Context();
        context.submit(impl->commands);
        context.waitIdle();
        return;
    }

    if (impl->useMsaa() && backBuffer != nullptr)
    {
        // The MSAA target lives in RENDER_TARGET state between frames; the
        // back buffer never left PRESENT (the pass rendered into the MSAA
        // target), so both transition just around the resolve.
        transition(list,
                   impl->msaa->texture,
                   D3D12_RESOURCE_STATE_RENDER_TARGET,
                   D3D12_RESOURCE_STATE_RESOLVE_SOURCE);
        transition(list,
                   backBuffer,
                   D3D12_RESOURCE_STATE_PRESENT,
                   D3D12_RESOURCE_STATE_RESOLVE_DEST);

        list->ResolveSubresource(
            backBuffer, 0, impl->msaa->texture, 0, impl->msaa->format);

        transition(list,
                   impl->msaa->texture,
                   D3D12_RESOURCE_STATE_RESOLVE_SOURCE,
                   D3D12_RESOURCE_STATE_RENDER_TARGET);
        transition(list,
                   backBuffer,
                   D3D12_RESOURCE_STATE_RESOLVE_DEST,
                   D3D12_RESOURCE_STATE_PRESENT);
    }
    else if (impl->passBegun && backBuffer != nullptr)
    {
        transition(list,
                   backBuffer,
                   D3D12_RESOURCE_STATE_RENDER_TARGET,
                   D3D12_RESOURCE_STATE_PRESENT);
    }

    getD3D12Context().submit(impl->commands);

    if (impl->drawable->swapChain != nullptr)
        impl->drawable->swapChain->Present(1, 0);
}

RenderPass Frame::beginPass(const RenderPassDescriptor& descriptor)
{
    if (impl->commands == nullptr || impl->drawable == nullptr
        || impl->drawable->backBuffer == nullptr)
        return RenderPass(nullptr);

    auto& context = getD3D12Context();
    auto* list = impl->commands->list.get();

    impl->bindRootState(context, list);

    // The off-screen colour texture is created already in RENDER_TARGET; only a
    // swapchain back buffer starts in PRESENT and needs promoting here.
    if (!impl->useMsaa() && !impl->passBegun && !impl->offscreen)
        transition(list,
                   impl->drawable->backBuffer,
                   D3D12_RESOURCE_STATE_PRESENT,
                   D3D12_RESOURCE_STATE_RENDER_TARGET);

    impl->passBegun = true;

    auto target =
        impl->useMsaa() ? impl->msaa->view : impl->drawable->backBufferView;
    auto hasDepth = impl->depth != nullptr && impl->depth->view.ptr != 0;

    list->OMSetRenderTargets(
        1, &target, FALSE, hasDepth ? &impl->depth->view : nullptr);

    D3D12_VIEWPORT viewport = {};
    viewport.Width = static_cast<float>(impl->drawable->width);
    viewport.Height = static_cast<float>(impl->drawable->height);
    viewport.MaxDepth = 1.0f;
    list->RSSetViewports(1, &viewport);

    // D3D12 has no default scissor; an unset one clips everything away.
    D3D12_RECT scissor = {0,
                          0,
                          static_cast<LONG>(impl->drawable->width),
                          static_cast<LONG>(impl->drawable->height)};
    list->RSSetScissorRects(1, &scissor);

    if (descriptor.clear)
    {
        const auto& color = descriptor.clearColor;
        const float clearColor[4] = {color.r, color.g, color.b, color.a};
        list->ClearRenderTargetView(target, clearColor, 0, nullptr);
    }

    // Depth is cleared to the far plane (1.0) whenever a depth buffer is
    // bound, matching the Metal pass's unconditional depth clear.
    if (hasDepth)
        list->ClearDepthStencilView(
            impl->depth->view, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);

    // The pass carries the target's pixel size so it can clamp scissor rects.
    return RenderPass(new D3D12Encoder {impl->commands, {}},
                      static_cast<int>(impl->drawable->width),
                      static_cast<int>(impl->drawable->height));
}

// Rendering into an app-owned texture: one attachment and no resolve. Depth is
// the target's own, from TextureDescriptor::depth, and rests in DEPTH_WRITE for
// its lifetime, so it costs a clear here and no barrier.
//
// Deliberately does not touch passBegun, which records whether the *back
// buffer* was moved out of PRESENT - a frame whose only passes were into
// textures must not have one transitioned back on the way out.
//
// The texture is moved into RENDER_TARGET here and moved back the moment
// something samples it, in RenderPass::setFragmentTexture, rather than at the
// end of the pass: a target written by one pass and read by the next then costs
// exactly the two barriers it needs, and one written and never read costs one.
RenderPass Frame::beginPass(const Texture& target,
                            const RenderPassDescriptor& descriptor)
{
    auto* data = static_cast<D3D12TextureData*>(target.nativeTexture());

    if (impl->commands == nullptr || data == nullptr || !target.isRenderTarget())
        return RenderPass(nullptr);

    auto& context = getD3D12Context();
    auto* list = impl->commands->list.get();

    impl->bindRootState(context, list);
    transitionTextureForUse(list, *data, D3D12_RESOURCE_STATE_RENDER_TARGET);

    auto hasDepth = data->hasDepth();
    list->OMSetRenderTargets(1, &data->rtv, FALSE, hasDepth ? &data->dsv : nullptr);

    auto width = target.width();
    auto height = target.height();

    D3D12_VIEWPORT viewport = {};
    viewport.Width = static_cast<float>(width);
    viewport.Height = static_cast<float>(height);
    viewport.MaxDepth = 1.0f;
    list->RSSetViewports(1, &viewport);

    // D3D12 has no default scissor; an unset one clips everything away.
    D3D12_RECT scissor = {0, 0, static_cast<LONG>(width), static_cast<LONG>(height)};
    list->RSSetScissorRects(1, &scissor);

    if (descriptor.clear)
    {
        const auto& color = descriptor.clearColor;
        const float clearColor[4] = {color.r, color.g, color.b, color.a};
        list->ClearRenderTargetView(data->rtv, clearColor, 0, nullptr);
    }

    // Cleared to the far plane whenever there is one, matching both the
    // drawable pass here and the Metal pass's unconditional depth clear.
    if (hasDepth)
        list->ClearDepthStencilView(
            data->dsv, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);

    return RenderPass(new D3D12Encoder {impl->commands, {}}, width, height);
}

// A compute pass on the frame's own recording. The graphics and compute root
// signatures occupy separate slots on a command list, so binding one here does
// not disturb the render state beginPass set - the two kinds of pass interleave
// on one list without either having to restore anything.
//
// A buffer this pass writes as a UAV and a later pass binds as vertex data is
// transitioned by RenderPass::setVertexBuffer: same recording, so the per-
// recording state tracking sees the UAV state and emits the barrier.
ComputePass Frame::beginCompute()
{
    if (impl->commands == nullptr)
        return ComputePass(nullptr);

    bindComputeRootState(getD3D12Context(), impl->commands->list.get());

    return ComputePass(new D3D12ComputeEncoder {impl->commands});
}

bool Frame::isValid() const
{
    return impl->commands != nullptr && impl->drawable != nullptr
           && impl->drawable->backBuffer != nullptr;
}
} // namespace eacp::GPU

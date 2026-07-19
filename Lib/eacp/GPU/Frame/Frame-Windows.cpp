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

    // The root signature and heaps are fixed for every render pipeline, so
    // binding them here frees the pass from caring about call ordering.
    ID3D12DescriptorHeap* heaps[] = {context.getTextureHeap(),
                                     context.getSamplerHeap()};
    list->SetDescriptorHeaps(2, heaps);
    list->SetGraphicsRootSignature(context.getRenderRootSignature());

    // Resource Binding Tier 1 hardware requires *every* descriptor table the
    // root signature declares to be populated before a draw, even the ones the
    // shader never reads — an unset table drops the draw entirely rather than
    // failing loudly. The signature is shared and declares maxTextureSlots of
    // them, while a typical shader binds one, so the rest are seeded with the
    // null descriptors here; setFragmentTexture overwrites the slots that carry
    // a real texture.
    //
    // Tier 2+ hardware ignores unset tables, which is why this only ever showed
    // up on an Arm laptop: no text drew, and nothing was logged without the
    // D3D12 validation layer installed.
    const auto nullTexture = context.getNullTextureDescriptor();
    const auto nullSampler = context.getNullSamplerDescriptor();

    if (nullTexture.ptr != 0 && nullSampler.ptr != 0)
    {
        for (auto slot = 0; slot < maxTextureSlots; ++slot)
        {
            list->SetGraphicsRootDescriptorTable(renderTextureParam(slot),
                                                 nullTexture);
            list->SetGraphicsRootDescriptorTable(renderSamplerParam(slot),
                                                 nullSampler);
        }
    }

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

bool Frame::isValid() const
{
    return impl->commands != nullptr && impl->drawable != nullptr
           && impl->drawable->backBuffer != nullptr;
}
} // namespace eacp::GPU

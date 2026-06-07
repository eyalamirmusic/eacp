#include <eacp/Core/Utils/WinInclude.h>

#include "Frame.h"

#include "../Device/Device.h"
#include "../Windows/D3DTypes.h"

#include <d3d11.h>

// Windows/D3D11 backend. The drawable and msaaTexture handles point at the
// D3DDrawable / D3DMsaaTarget owned by GPUView. beginPass binds the target and
// clears it; the destructor resolves any multisample target into the back buffer
// and presents the swapchain (mirroring the Metal frame's present-on-destroy).

namespace eacp::GPU
{
struct Frame::Native
{
    Native(Device& device,
           void* drawableHandle,
           void* msaaTextureHandle,
           void* /*depthTextureHandle*/)
        : context(static_cast<ID3D11DeviceContext*>(device.nativeQueue()))
        , drawable(static_cast<D3DDrawable*>(drawableHandle))
        , msaa(static_cast<D3DMsaaTarget*>(msaaTextureHandle))
    {
        // Depth buffering is currently implemented on the Metal backend only; the
        // D3D11 path ignores the depth texture for now.
    }

    ID3D11RenderTargetView* targetView() const
    {
        if (msaa != nullptr && msaa->view != nullptr)
            return msaa->view;

        return drawable != nullptr ? drawable->backBufferView : nullptr;
    }

    ID3D11DeviceContext* context = nullptr;
    D3DDrawable* drawable = nullptr;
    D3DMsaaTarget* msaa = nullptr;
};

Frame::Frame(Device& device, void* drawable, void* msaaTexture, void* depthTexture)
    : impl(device, drawable, msaaTexture, depthTexture)
{
}

Frame::~Frame()
{
    if (impl->context == nullptr || impl->drawable == nullptr)
        return;

    if (impl->msaa != nullptr && impl->msaa->texture != nullptr
        && impl->drawable->backBuffer != nullptr)
    {
        impl->context->ResolveSubresource(impl->drawable->backBuffer,
                                          0,
                                          impl->msaa->texture,
                                          0,
                                          impl->msaa->format);
    }

    if (impl->drawable->swapChain != nullptr)
        impl->drawable->swapChain->Present(1, 0);
}

RenderPass Frame::beginPass(const RenderPassDescriptor& descriptor)
{
    auto* target = impl->targetView();

    if (impl->context == nullptr || target == nullptr)
        return RenderPass(nullptr);

    impl->context->OMSetRenderTargets(1, &target, nullptr);

    D3D11_VIEWPORT viewport = {};
    viewport.Width = static_cast<float>(impl->drawable->width);
    viewport.Height = static_cast<float>(impl->drawable->height);
    viewport.MaxDepth = 1.0f;
    impl->context->RSSetViewports(1, &viewport);

    if (descriptor.clear)
    {
        const auto& color = descriptor.clearColor;
        const float clearColor[4] = {color.r, color.g, color.b, color.a};
        impl->context->ClearRenderTargetView(target, clearColor);
    }

    return RenderPass(new D3DEncoder {impl->context, 0});
}

bool Frame::isValid() const
{
    return impl->context != nullptr && impl->drawable != nullptr
           && impl->drawable->backBufferView != nullptr;
}
} // namespace eacp::GPU

#include <eacp/Core/Utils/WinInclude.h>

#include "GPUView.h"

#include "../Device/Device.h"
#include "../Frame/Frame.h"
#include "../Windows/D3DTypes.h"

#include <eacp/Core/Utils/Containers.h>
#include <eacp/Graphics/Helpers/DisplayLink.h>

#include <d3d11.h>
#include <dxgi1_2.h>

#include <winrt/Windows.UI.Composition.h>
#include <windows.ui.composition.interop.h>

namespace wuc = winrt::Windows::UI::Composition;

namespace eacp::Graphics
{
// Defined in Graphics/D2DFactory-Windows.cpp (linked via eacp-graphics).
wuc::Compositor getWinRTCompositor();
} // namespace eacp::Graphics

namespace eacp::GPU
{
// Backs the GPUView with a SpriteVisual whose brush samples a composition
// swapchain, added under the standard View ContainerVisual so it lives in the
// normal Windows.UI.Composition visual tree. Renders into the swapchain back
// buffer (resolving an MSAA target into it when enabled) and presents.
struct GPUView::Native
{
    explicit Native(GPUView& viewToUse)
        : view(viewToUse)
    {
        compositor = Graphics::getWinRTCompositor();
        device = static_cast<ID3D11Device*>(Device::shared().nativeDevice());

        if (!compositor || device == nullptr)
            return;

        spriteVisual = compositor.CreateSpriteVisual();

        if (auto* container =
                static_cast<wuc::ContainerVisual*>(view.getNativeLayer()))
            if (*container)
                container->Children().InsertAtTop(spriteVisual);
    }

    ~Native()
    {
        stopContinuous();

        if (spriteVisual)
            if (auto* container =
                    static_cast<wuc::ContainerVisual*>(view.getNativeLayer()))
                if (*container)
                    container->Children().Remove(spriteVisual);
    }

    static float dpiScale() { return static_cast<float>(GetDpiForSystem()) / 96.f; }

    void updateSize()
    {
        if (!compositor || device == nullptr)
            return;

        auto bounds = view.getLocalBounds();
        auto scale = dpiScale();
        width = static_cast<UINT>(bounds.w * scale);
        height = static_cast<UINT>(bounds.h * scale);

        if (width == 0 || height == 0)
            return;

        if (!swapChain)
            createSwapChain();
        else
            resizeSwapChain();

        if (spriteVisual)
            spriteVisual.Size({bounds.w, bounds.h});

        updateMultisampleTexture();
        updateDepthTexture();
    }

    void createSwapChain()
    {
        auto dxgiDevice = winrt::com_ptr<IDXGIDevice>();
        if (FAILED(device->QueryInterface(__uuidof(IDXGIDevice),
                                          dxgiDevice.put_void())))
            return;

        auto adapter = winrt::com_ptr<IDXGIAdapter>();
        if (FAILED(dxgiDevice->GetAdapter(adapter.put())))
            return;

        auto factory = winrt::com_ptr<IDXGIFactory2>();
        if (FAILED(adapter->GetParent(__uuidof(IDXGIFactory2), factory.put_void())))
            return;

        DXGI_SWAP_CHAIN_DESC1 descriptor = {};
        descriptor.Width = width;
        descriptor.Height = height;
        descriptor.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        descriptor.SampleDesc.Count = 1;
        descriptor.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        descriptor.BufferCount = 2;
        descriptor.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
        descriptor.AlphaMode = DXGI_ALPHA_MODE_IGNORE;

        if (FAILED(factory->CreateSwapChainForComposition(
                device, &descriptor, nullptr, swapChain.put())))
            return;

        attachSwapChainToVisual();
        createBackBufferView();
    }

    void attachSwapChainToVisual()
    {
        namespace abi = ABI::Windows::UI::Composition;

        auto interop = compositor.as<abi::ICompositorInterop>();
        auto abiSurface = winrt::com_ptr<abi::ICompositionSurface>();

        if (FAILED(interop->CreateCompositionSurfaceForSwapChain(swapChain.get(),
                                                                 abiSurface.put())))
            return;

        auto surface = abiSurface.as<wuc::ICompositionSurface>();
        auto brush = compositor.CreateSurfaceBrush(surface);
        brush.Stretch(wuc::CompositionStretch::Fill);

        if (spriteVisual)
            spriteVisual.Brush(brush);
    }

    void createBackBufferView()
    {
        backBufferView = nullptr;
        backBuffer = nullptr;

        if (!swapChain)
            return;

        if (FAILED(swapChain->GetBuffer(
                0, __uuidof(ID3D11Texture2D), backBuffer.put_void())))
            return;

        device->CreateRenderTargetView(
            backBuffer.get(), nullptr, backBufferView.put());
    }

    void resizeSwapChain()
    {
        backBufferView = nullptr;
        backBuffer = nullptr;

        if (FAILED(
                swapChain->ResizeBuffers(0, width, height, DXGI_FORMAT_UNKNOWN, 0)))
            return;

        createBackBufferView();
    }

    void updateMultisampleTexture()
    {
        msaaView = nullptr;
        msaaTexture = nullptr;

        if (sampleCount <= 1 || width == 0 || height == 0 || device == nullptr)
            return;

        UINT quality = 0;
        device->CheckMultisampleQualityLevels(
            DXGI_FORMAT_B8G8R8A8_UNORM, static_cast<UINT>(sampleCount), &quality);
        if (quality == 0)
            return;

        D3D11_TEXTURE2D_DESC descriptor = {};
        descriptor.Width = width;
        descriptor.Height = height;
        descriptor.MipLevels = 1;
        descriptor.ArraySize = 1;
        descriptor.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        descriptor.SampleDesc.Count = static_cast<UINT>(sampleCount);
        descriptor.Usage = D3D11_USAGE_DEFAULT;
        descriptor.BindFlags = D3D11_BIND_RENDER_TARGET;

        device->CreateTexture2D(&descriptor, nullptr, msaaTexture.put());

        if (msaaTexture)
            device->CreateRenderTargetView(
                msaaTexture.get(), nullptr, msaaView.put());
    }

    // Depth buffer sized to the target. Its sample count must match the colour
    // target actually in use (the resolved MSAA texture, or the back buffer), so
    // it keys off the same condition render() uses to pick the colour target.
    void updateDepthTexture()
    {
        depthView = nullptr;
        depthTexture = nullptr;

        if (!depthEnabled || width == 0 || height == 0 || device == nullptr)
            return;

        auto useMsaa = sampleCount > 1 && msaaView;

        D3D11_TEXTURE2D_DESC descriptor = {};
        descriptor.Width = width;
        descriptor.Height = height;
        descriptor.MipLevels = 1;
        descriptor.ArraySize = 1;
        descriptor.Format = DXGI_FORMAT_D32_FLOAT;
        descriptor.SampleDesc.Count = useMsaa ? static_cast<UINT>(sampleCount) : 1;
        descriptor.Usage = D3D11_USAGE_DEFAULT;
        descriptor.BindFlags = D3D11_BIND_DEPTH_STENCIL;

        device->CreateTexture2D(&descriptor, nullptr, depthTexture.put());

        if (!depthTexture)
            return;

        D3D11_DEPTH_STENCIL_VIEW_DESC viewDescriptor = {};
        viewDescriptor.Format = DXGI_FORMAT_D32_FLOAT;
        viewDescriptor.ViewDimension = useMsaa ? D3D11_DSV_DIMENSION_TEXTURE2DMS
                                               : D3D11_DSV_DIMENSION_TEXTURE2D;
        device->CreateDepthStencilView(
            depthTexture.get(), &viewDescriptor, depthView.put());
    }

    void render()
    {
        if (!swapChain || !backBufferView || width == 0 || height == 0)
            return;

        D3DDrawable drawable = {};
        drawable.swapChain = swapChain.get();
        drawable.backBuffer = backBuffer.get();
        drawable.backBufferView = backBufferView.get();
        drawable.width = width;
        drawable.height = height;

        auto useMsaa = sampleCount > 1 && msaaView;

        D3DMsaaTarget msaa = {};
        if (useMsaa)
        {
            msaa.texture = msaaTexture.get();
            msaa.view = msaaView.get();
        }

        auto useDepth = depthEnabled && depthView;

        D3DDepthTarget depth = {};
        if (useDepth)
        {
            depth.texture = depthTexture.get();
            depth.view = depthView.get();
        }

        auto frame = Frame(Device::shared(),
                           &drawable,
                           useMsaa ? &msaa : nullptr,
                           useDepth ? &depth : nullptr);
        view.render(frame);
    }

    void startContinuous()
    {
        if (displayLink == nullptr)
            displayLink =
                makeOwned<Threads::DisplayLink>([this] { view.renderNow(); });
    }

    void stopContinuous() { displayLink = nullptr; }

    GPUView& view;
    int sampleCount = 4;
    bool continuous = false;
    bool depthEnabled = false;
    UINT width = 0;
    UINT height = 0;

    wuc::Compositor compositor {nullptr};
    wuc::SpriteVisual spriteVisual {nullptr};
    ID3D11Device* device = nullptr;

    winrt::com_ptr<IDXGISwapChain1> swapChain;
    winrt::com_ptr<ID3D11Texture2D> backBuffer;
    winrt::com_ptr<ID3D11RenderTargetView> backBufferView;
    winrt::com_ptr<ID3D11Texture2D> msaaTexture;
    winrt::com_ptr<ID3D11RenderTargetView> msaaView;
    winrt::com_ptr<ID3D11Texture2D> depthTexture;
    winrt::com_ptr<ID3D11DepthStencilView> depthView;

    OwningPointer<Threads::DisplayLink> displayLink;
};

GPUView::GPUView()
    : impl(*this)
{
}

GPUView::~GPUView() = default;

int GPUView::sampleCount() const
{
    return impl->sampleCount;
}

void GPUView::setSampleCount(int count)
{
    impl->sampleCount = count;
    impl->updateMultisampleTexture();
    impl->updateDepthTexture();
}

void GPUView::setDepth(bool enabled)
{
    impl->depthEnabled = enabled;
    impl->updateDepthTexture();
}

bool GPUView::hasDepth() const
{
    return impl->depthEnabled;
}

void GPUView::setContinuous(bool continuous)
{
    impl->continuous = continuous;

    if (continuous)
        impl->startContinuous();
    else
        impl->stopContinuous();
}

bool GPUView::isContinuous() const
{
    return impl->continuous;
}

void GPUView::resized()
{
    Graphics::View::resized();
    impl->updateSize();
    repaint();
}

void GPUView::paint(Graphics::Context&)
{
    renderNow();
}

void GPUView::renderNow()
{
    impl->render();
}
} // namespace eacp::GPU

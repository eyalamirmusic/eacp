#include <eacp/Core/Utils/WinInclude.h>

#include "GPUView.h"

#include "../Device/Device.h"
#include "../Frame/Frame.h"
#include "../Windows/D3D12Types.h"

#include <eacp/Core/Threads/EventLoop.h>
#include <eacp/Core/Utils/Containers.h>
#include <eacp/Graphics/Helpers/DisplayLink.h>

#include <dxgi1_4.h>

#include <array>
#include <unordered_set>

#include <winrt/Windows.UI.Composition.h>
#include <windows.ui.composition.interop.h>

namespace wuc = winrt::Windows::UI::Composition;

namespace eacp::Graphics
{
// Defined in Graphics/D2DFactory-Windows.cpp (linked via eacp-graphics).
wuc::Compositor getWinRTCompositor();
bool handleDeviceLossIfNeeded(HRESULT hr);
} // namespace eacp::Graphics

namespace eacp::GPU
{
namespace
{
// Two buffers keep one frame in flight while the next records; the per-buffer
// fence in render() stops the CPU from reusing a buffer the GPU still reads.
constexpr UINT bufferCount = 2;

// Lets the device-loss refresh reach every live GPUView without naming the
// private GPUView::Native type (same pattern as View-Windows' PaintTarget).
struct DeviceResourceHolder
{
    virtual ~DeviceResourceHolder() = default;
    virtual void recreateDeviceResources() = 0;
};

// Main-thread only, so no locking is needed.
std::unordered_set<DeviceResourceHolder*>& liveGPUViews()
{
    static auto views = std::unordered_set<DeviceResourceHolder*> {};
    return views;
}
} // namespace

// Called by Device::Native after the D3D12 device was rebuilt following
// device removal: every swapchain was created on the dead device, so each
// view rebuilds.
void refreshAllGPUViewsForNewDevice()
{
    for (auto* view: liveGPUViews())
        view->recreateDeviceResources();
}

// Backs the GPUView with a SpriteVisual whose brush samples a composition
// swapchain created from the D3D12 direct queue, added under the standard
// View ContainerVisual so it lives in the normal Windows.UI.Composition
// visual tree. Renders into the swapchain back buffer (resolving an MSAA
// target into it when enabled) and presents.
struct GPUView::Native : DeviceResourceHolder
{
    explicit Native(GPUView& viewToUse)
        : view(viewToUse)
    {
        liveGPUViews().insert(this);

        compositor = Graphics::getWinRTCompositor();
        device = static_cast<ID3D12Device*>(Device::shared().nativeDevice());

        if (!compositor || device == nullptr)
            return;

        spriteVisual = compositor.CreateSpriteVisual();

        if (auto* container =
                static_cast<wuc::ContainerVisual*>(view.getNativeLayer()))
            if (*container)
                container->Children().InsertAtTop(spriteVisual);
    }

    ~Native() override
    {
        liveGPUViews().erase(this);
        stopContinuous();

        // The last frames may still reference the back buffers and targets
        // about to be released.
        getD3D12Context().waitIdle();

        if (spriteVisual)
            if (auto* container =
                    static_cast<wuc::ContainerVisual*>(view.getNativeLayer()))
                if (*container)
                    container->Children().Remove(spriteVisual);
    }

    // Drops every resource created on the lost device, re-acquires the
    // replacement and rebuilds the swapchain at the current size. App-owned
    // resources rebuild through the view's onDeviceRestored hook.
    void recreateDeviceResources() override
    {
        for (auto& buffer: backBuffers)
            buffer = nullptr;

        msaaTexture = nullptr;
        depthTexture = nullptr;
        rtvHeap = nullptr;
        dsvHeap = nullptr;
        swapChain = nullptr;
        frameFences = {};

        if (spriteVisual)
            spriteVisual.Brush(nullptr);

        device = static_cast<ID3D12Device*>(Device::shared().nativeDevice());

        if (device != nullptr)
            updateSize();

        view.onDeviceRestored();
        view.repaint();
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
        auto& context = getD3D12Context();

        if (!context.isValid())
            return;

        auto factory = winrt::com_ptr<IDXGIFactory2>();
        if (FAILED(
                CreateDXGIFactory2(0, __uuidof(IDXGIFactory2), factory.put_void())))
            return;

        DXGI_SWAP_CHAIN_DESC1 descriptor = {};
        descriptor.Width = width;
        descriptor.Height = height;
        descriptor.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        descriptor.SampleDesc.Count = 1;
        descriptor.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        descriptor.BufferCount = bufferCount;
        descriptor.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
        descriptor.AlphaMode = DXGI_ALPHA_MODE_IGNORE;

        // A D3D12 swapchain is created from the command queue, not the device.
        auto chain = winrt::com_ptr<IDXGISwapChain1>();
        if (FAILED(factory->CreateSwapChainForComposition(
                context.getQueue(), &descriptor, nullptr, chain.put())))
            return;

        swapChain = chain.try_as<IDXGISwapChain3>();

        if (!swapChain)
            return;

        attachSwapChainToVisual();
        createDescriptorHeaps();
        createBackBufferViews();
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

    void createDescriptorHeaps()
    {
        if (rtvHeap)
            return;

        D3D12_DESCRIPTOR_HEAP_DESC rtvDesc = {};
        rtvDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        rtvDesc.NumDescriptors = bufferCount + 1; // back buffers + MSAA target

        if (FAILED(device->CreateDescriptorHeap(
                &rtvDesc, __uuidof(ID3D12DescriptorHeap), rtvHeap.put_void())))
            return;

        auto increment =
            device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
        auto start = rtvHeap->GetCPUDescriptorHandleForHeapStart();

        for (auto i = UINT {0}; i < bufferCount; ++i)
        {
            rtvHandles[i] = start;
            rtvHandles[i].ptr += static_cast<SIZE_T>(i) * increment;
        }

        msaaViewHandle = start;
        msaaViewHandle.ptr += static_cast<SIZE_T>(bufferCount) * increment;

        D3D12_DESCRIPTOR_HEAP_DESC dsvDesc = {};
        dsvDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
        dsvDesc.NumDescriptors = 1;

        if (SUCCEEDED(device->CreateDescriptorHeap(
                &dsvDesc, __uuidof(ID3D12DescriptorHeap), dsvHeap.put_void())))
            depthViewHandle = dsvHeap->GetCPUDescriptorHandleForHeapStart();
    }

    void createBackBufferViews()
    {
        if (!swapChain || !rtvHeap)
            return;

        for (auto i = UINT {0}; i < bufferCount; ++i)
        {
            backBuffers[i] = nullptr;

            if (FAILED(swapChain->GetBuffer(
                    i, __uuidof(ID3D12Resource), backBuffers[i].put_void())))
                return;

            device->CreateRenderTargetView(
                backBuffers[i].get(), nullptr, rtvHandles[i]);
        }
    }

    void resizeSwapChain()
    {
        // The buffers being replaced may still be referenced by an in-flight
        // frame, and ResizeBuffers requires every outstanding reference gone.
        getD3D12Context().waitIdle();

        for (auto& buffer: backBuffers)
            buffer = nullptr;

        frameFences = {};

        if (FAILED(swapChain->ResizeBuffers(
                bufferCount, width, height, DXGI_FORMAT_UNKNOWN, 0)))
            return;

        createBackBufferViews();
    }

    void updateMultisampleTexture()
    {
        getD3D12Context().deferRelease(std::move(msaaTexture));

        if (sampleCount <= 1 || width == 0 || height == 0 || device == nullptr
            || !rtvHeap)
            return;

        D3D12_FEATURE_DATA_MULTISAMPLE_QUALITY_LEVELS levels = {};
        levels.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        levels.SampleCount = static_cast<UINT>(sampleCount);

        if (FAILED(device->CheckFeatureSupport(
                D3D12_FEATURE_MULTISAMPLE_QUALITY_LEVELS, &levels, sizeof(levels)))
            || levels.NumQualityLevels == 0)
            return;

        D3D12_HEAP_PROPERTIES heap = {};
        heap.Type = D3D12_HEAP_TYPE_DEFAULT;

        D3D12_RESOURCE_DESC descriptor = {};
        descriptor.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        descriptor.Width = width;
        descriptor.Height = height;
        descriptor.DepthOrArraySize = 1;
        descriptor.MipLevels = 1;
        descriptor.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        descriptor.SampleDesc.Count = static_cast<UINT>(sampleCount);
        descriptor.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;

        if (FAILED(
                device->CreateCommittedResource(&heap,
                                                D3D12_HEAP_FLAG_NONE,
                                                &descriptor,
                                                D3D12_RESOURCE_STATE_RENDER_TARGET,
                                                nullptr,
                                                __uuidof(ID3D12Resource),
                                                msaaTexture.put_void())))
            return;

        device->CreateRenderTargetView(msaaTexture.get(), nullptr, msaaViewHandle);
    }

    // Depth buffer sized to the target. Its sample count must match the colour
    // target actually in use (the MSAA texture, or the back buffer), so it
    // keys off the same condition render() uses to pick the colour target.
    void updateDepthTexture()
    {
        getD3D12Context().deferRelease(std::move(depthTexture));

        if (!depthEnabled || width == 0 || height == 0 || device == nullptr
            || !dsvHeap)
            return;

        auto useMsaa = sampleCount > 1 && msaaTexture != nullptr;

        D3D12_HEAP_PROPERTIES heap = {};
        heap.Type = D3D12_HEAP_TYPE_DEFAULT;

        D3D12_RESOURCE_DESC descriptor = {};
        descriptor.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        descriptor.Width = width;
        descriptor.Height = height;
        descriptor.DepthOrArraySize = 1;
        descriptor.MipLevels = 1;
        descriptor.Format = DXGI_FORMAT_D32_FLOAT;
        descriptor.SampleDesc.Count = useMsaa ? static_cast<UINT>(sampleCount) : 1;
        descriptor.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;

        D3D12_CLEAR_VALUE clearValue = {};
        clearValue.Format = DXGI_FORMAT_D32_FLOAT;
        clearValue.DepthStencil.Depth = 1.0f;

        if (FAILED(device->CreateCommittedResource(&heap,
                                                   D3D12_HEAP_FLAG_NONE,
                                                   &descriptor,
                                                   D3D12_RESOURCE_STATE_DEPTH_WRITE,
                                                   &clearValue,
                                                   __uuidof(ID3D12Resource),
                                                   depthTexture.put_void())))
            return;

        device->CreateDepthStencilView(depthTexture.get(), nullptr, depthViewHandle);
    }

    void render()
    {
        auto& context = getD3D12Context();

        if (!swapChain || !context.isValid() || width == 0 || height == 0)
            return;

        auto index = swapChain->GetCurrentBackBufferIndex();

        if (index >= bufferCount || backBuffers[index] == nullptr)
            return;

        // Block until the GPU released this buffer's previous frame, keeping
        // at most one frame in flight per buffer.
        context.waitFor(frameFences[index]);

        D3D12Drawable drawable = {};
        drawable.swapChain = swapChain.get();
        drawable.backBuffer = backBuffers[index].get();
        drawable.backBufferView = rtvHandles[index];
        drawable.width = width;
        drawable.height = height;

        auto useMsaa = sampleCount > 1 && msaaTexture != nullptr;

        D3D12MsaaTarget msaa = {};
        if (useMsaa)
        {
            msaa.texture = msaaTexture.get();
            msaa.view = msaaViewHandle;
        }

        auto useDepth = depthEnabled && depthTexture != nullptr;

        D3D12DepthTarget depth = {};
        if (useDepth)
            depth.view = depthViewHandle;

        {
            auto frame = Frame(Device::shared(),
                               &drawable,
                               useMsaa ? &msaa : nullptr,
                               useDepth ? &depth : nullptr);
            view.render(frame);
        }

        frameFences[index] = context.lastSubmitted();
        checkDeviceRemoved();
    }

    void checkDeviceRemoved()
    {
        if (device == nullptr || SUCCEEDED(device->GetDeviceRemovedReason()))
            return;

        // Recovery rebuilds this view's swapchain, so run it from a fresh
        // stack frame instead of re-entering while render() is live. The 2D
        // layer's recovery fires the listener that rebuilds the GPU device.
        Threads::callAsync(
            [] { Graphics::handleDeviceLossIfNeeded(DXGI_ERROR_DEVICE_REMOVED); });
    }

    // The tick only advances animation state and invalidates; the render
    // itself runs from the WM_PAINT this schedules. WM_PAINT is delivered
    // only when the message queue is otherwise empty, so the heavy work
    // (rendering, and a Present that can block on the compositor) can never
    // starve input or other queued work, no matter how slow frames get —
    // modal size/move loops included, where paints keep flowing between
    // mouse moves and the animation keeps running during a live resize.
    void startContinuous()
    {
        if (displayLink == nullptr)
            displayLink = makeOwned<Threads::DisplayLink>(
                [this](Threads::FrameTime time)
                {
                    view.update(time);
                    view.repaint();
                });
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
    ID3D12Device* device = nullptr;

    winrt::com_ptr<IDXGISwapChain3> swapChain;
    std::array<winrt::com_ptr<ID3D12Resource>, bufferCount> backBuffers;
    std::array<D3D12_CPU_DESCRIPTOR_HANDLE, bufferCount> rtvHandles = {};
    std::array<std::uint64_t, bufferCount> frameFences = {};

    winrt::com_ptr<ID3D12DescriptorHeap> rtvHeap;
    winrt::com_ptr<ID3D12DescriptorHeap> dsvHeap;
    D3D12_CPU_DESCRIPTOR_HANDLE msaaViewHandle = {};
    D3D12_CPU_DESCRIPTOR_HANDLE depthViewHandle = {};

    winrt::com_ptr<ID3D12Resource> msaaTexture;
    winrt::com_ptr<ID3D12Resource> depthTexture;

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

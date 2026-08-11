#include "GPUView.h"
#include <eacp/Graphics/DComp-Windows.h>
#include <eacp/Graphics/Window/CompositionHostWindow-Windows.h>

#include "../Device/Device.h"
#include "../Frame/Frame.h"
#include "../Windows/D3D12Types.h"

#include <eacp/Core/Utils/Singleton.h>
#include <eacp/Graphics/Image/Image.h>

#include <algorithm>
#include <cmath>
#include <unordered_set>

namespace eacp::GPU
{
namespace
{
// One buffer per in-flight frame: shown, queued and being drawn. DXGI is never
// asked to hold more frames than there are buffers for.
constexpr int maxFramesInFlight = 3;
constexpr UINT bufferCount = maxFramesInFlight;

// Lets the device-loss refresh reach every live GPUView without naming the
// private GPUView::Native type.
struct DeviceResourceHolder
{
    virtual ~DeviceResourceHolder() = default;
    virtual void recreateDeviceResources() = 0;
};

// Main-thread only, so no locking. Immortal because ~Native erases itself here
// and a namespace-scope GPUView outlives a lazily constructed registry.
std::unordered_set<DeviceResourceHolder*>& liveGPUViews()
{
    return Singleton::getImmortal<std::unordered_set<DeviceResourceHolder*>>();
}
} // namespace

// Called by Device::Native after a device rebuild: every swapchain was created
// on the dead device.
void refreshAllGPUViewsForNewDevice()
{
    for (auto* view: liveGPUViews())
        view->recreateDeviceResources();
}

struct GPUView::Native : DeviceResourceHolder
{
    explicit Native(GPUView& viewToUse)
        : view(viewToUse)
    {
        liveGPUViews().insert(this);

        compositionDevice = Graphics::getCompositionDevice();
        device = static_cast<ID3D12Device*>(Device::shared().nativeDevice());

        if (!compositionDevice || device == nullptr)
            return;

        if (FAILED(compositionDevice->CreateVisual(spriteVisual.GetAddressOf())))
        {
            spriteVisual.Reset();
            return;
        }

        if (auto* container =
                static_cast<IDCompositionVisual2*>(view.getNativeLayer()))
            Graphics::insertVisualAtTop(container, spriteVisual.Get());
    }

    ~Native() override
    {
        liveGPUViews().erase(this);
        stopContinuous();

        // In-flight frames may still reference the buffers about to be released.
        getD3D12Context().waitIdle();

        if (spriteVisual)
            if (auto* container =
                    static_cast<IDCompositionVisual2*>(view.getNativeLayer()))
                container->RemoveVisual(spriteVisual.Get());

        Graphics::commitComposition();
    }

    // App-owned resources rebuild through the view's onDeviceRestored hook.
    void recreateDeviceResources() override
    {
        for (auto& buffer: backBuffers)
            buffer = nullptr;

        msaaTexture = nullptr;
        depthTexture = nullptr;
        rtvHeap = nullptr;
        dsvHeap = nullptr;
        if (frameLatencyWaitable != nullptr)
        {
            CloseHandle(frameLatencyWaitable);
            frameLatencyWaitable = nullptr;
        }

        swapChain = nullptr;
        frameFences.fill(0);

        if (spriteVisual)
            spriteVisual->SetContent(nullptr);

        // The DComp device is replaced along with the rendering device.
        compositionDevice = Graphics::getCompositionDevice();
        spriteVisual.Reset();

        if (compositionDevice
            && SUCCEEDED(
                compositionDevice->CreateVisual(spriteVisual.GetAddressOf())))
        {
            if (auto* container =
                    static_cast<IDCompositionVisual2*>(view.getNativeLayer()))
                Graphics::insertVisualAtTop(container, spriteVisual.Get());
        }

        device = static_cast<ID3D12Device*>(Device::shared().nativeDevice());

        if (device != nullptr)
            updateSize();

        view.onDeviceRestored();
        view.repaint();
    }

    static float dpiScale() { return static_cast<float>(GetDpiForSystem()) / 96.f; }

    void updateSize()
    {
        if (!compositionDevice || device == nullptr)
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

        applyContentScale();

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

        // A transparent window composites straight to the screen, so its alpha
        // must reach DWM; IGNORE would fill it with an opaque black box.
        descriptor.AlphaMode =
            Graphics::isHostWindowTransparent(Graphics::findHostHwndForView(&view))
                ? DXGI_ALPHA_MODE_PREMULTIPLIED
                : DXGI_ALPHA_MODE_IGNORE;

        // Waitable, so the present queue's depth is ours to choose; otherwise
        // DXGI queues three frames. See GPUView::setFramesInFlight.
        descriptor.Flags = DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;

        // A D3D12 swapchain is created from the command queue, not the device.
        auto chain = winrt::com_ptr<IDXGISwapChain1>();
        if (FAILED(factory->CreateSwapChainForComposition(
                context.getQueue(), &descriptor, nullptr, chain.put())))
            return;

        swapChain = chain.try_as<IDXGISwapChain3>();

        if (!swapChain)
            return;

        applyFrameLatency();
        frameLatencyWaitable = swapChain->GetFrameLatencyWaitableObject();

        attachSwapChainToVisual();
        createDescriptorHeaps();
        createBackBufferViews();
    }

    // The swapchain is already sized in physical pixels, so the visual
    // counter-scales by 1/dpiScale to cancel the root's DPI transform (see
    // NativeLayer-Windows.h).
    void attachSwapChainToVisual()
    {
        if (!spriteVisual || !swapChain)
            return;

        spriteVisual->SetContent(swapChain.get());
        applyContentScale();
        Graphics::commitComposition();
    }

    void applyContentScale()
    {
        auto scale = dpiScale();

        if (spriteVisual && scale > 0.f)
            spriteVisual->SetTransform(
                D2D1::Matrix3x2F::Scale(1.f / scale, 1.f / scale));
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
        // ResizeBuffers requires every outstanding buffer reference gone.
        getD3D12Context().waitIdle();

        for (auto& buffer: backBuffers)
            buffer = nullptr;

        frameFences.fill(0);

        if (FAILED(swapChain->ResizeBuffers(
                bufferCount,
                width,
                height,
                DXGI_FORMAT_UNKNOWN,
                DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT)))
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

    // Keys off the same condition render() picks the colour target with, the
    // sample counts having to match.
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

    // A waitable swapchain defaults to one frame, which stalls the GPU on the
    // CPU between frames; Microsoft's guidance is two.
    void applyFrameLatency()
    {
        if (swapChain)
            swapChain->SetMaximumFrameLatency(static_cast<UINT>(framesInFlight));
    }

    void render()
    {
        auto& context = getD3D12Context();

        if (!swapChain || !context.isValid() || width == 0 || height == 0)
            return;

        // Blocks until the swapchain is ready, keeping the CPU no further ahead
        // of the display than framesInFlight allows.
        if (frameLatencyWaitable != nullptr)
            WaitForSingleObjectEx(frameLatencyWaitable, 1000, TRUE);

        auto index = swapChain->GetCurrentBackBufferIndex();

        if (index >= bufferCount || backBuffers[index] == nullptr)
            return;

        // At most one frame in flight per buffer.
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

        // Recovery rebuilds this view's swapchain, so it runs from a fresh
        // stack frame rather than re-entering while render() is live.
        Threads::callAsync(
            [] { Graphics::handleDeviceLossIfNeeded(DXGI_ERROR_DEVICE_REMOVED); });
    }

    // The tick only advances state and invalidates; rendering runs from the
    // WM_PAINT it schedules, which is delivered only on an empty message queue,
    // so slow frames can never starve input.
    void startContinuous()
    {
        if (displayLink == nullptr)
        {
            auto func = [this](Threads::FrameTime time)
            {
                view.update(time);
                view.repaint();
            };

            displayLink.create(func);
        }
    }

    void stopContinuous() { displayLink = nullptr; }

    GPUView& view;
    int sampleCount = 4;

    // Two, a refresh sooner than DXGI's own three. See setFramesInFlight.
    int framesInFlight = 2;
    HANDLE frameLatencyWaitable = nullptr;

    bool continuous = false;
    bool depthEnabled = false;
    UINT width = 0;
    UINT height = 0;

    IDCompositionDesktopDevice* compositionDevice = nullptr;
    Microsoft::WRL::ComPtr<IDCompositionVisual2> spriteVisual;
    ID3D12Device* device = nullptr;

    winrt::com_ptr<IDXGISwapChain3> swapChain;
    Array<winrt::com_ptr<ID3D12Resource>, bufferCount> backBuffers;
    Array<D3D12_CPU_DESCRIPTOR_HANDLE, bufferCount> rtvHandles = {};
    Array<std::uint64_t, bufferCount> frameFences = {};

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

void GPUView::setFramesInFlight(int count)
{
    impl->framesInFlight =
        count < 1 ? 1 : (count > maxFramesInFlight ? maxFramesInFlight : count);
    impl->applyFrameLatency();
}

int GPUView::framesInFlight() const
{
    return impl->framesInFlight;
}

void GPUView::resized()
{
    Graphics::View::resized();
    impl->updateSize();
    repaint();
}

void GPUView::backingScaleChanged()
{
    Graphics::View::backingScaleChanged();

    // Redrawn because the presented frame was built for the old scale.
    impl->updateSize();
    onBackingScaleChanged(backingScale());
    repaint();
}

float GPUView::backingScale() const
{
    return Native::dpiScale();
}

void GPUView::paint(Graphics::Context& context)
{
    // A snapshot goes through renderNativeContent; renderNow() would present an
    // on-screen frame as a side effect.
    if (context.isSnapshot())
        return;

    renderNow();
}

void GPUView::renderNow()
{
    impl->render();
}

namespace
{
winrt::com_ptr<ID3D12Resource>
    makeSnapshotTexture(ID3D12Device* device,
                        UINT width,
                        UINT height,
                        DXGI_FORMAT format,
                        UINT sampleCount,
                        D3D12_RESOURCE_FLAGS flags,
                        D3D12_RESOURCE_STATES initialState,
                        const D3D12_CLEAR_VALUE* clearValue)
{
    D3D12_HEAP_PROPERTIES heap = {};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;

    D3D12_RESOURCE_DESC descriptor = {};
    descriptor.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    descriptor.Width = width;
    descriptor.Height = height;
    descriptor.DepthOrArraySize = 1;
    descriptor.MipLevels = 1;
    descriptor.Format = format;
    descriptor.SampleDesc.Count = sampleCount;
    descriptor.Flags = flags;

    winrt::com_ptr<ID3D12Resource> resource;
    if (FAILED(device->CreateCommittedResource(&heap,
                                               D3D12_HEAP_FLAG_NONE,
                                               &descriptor,
                                               initialState,
                                               clearValue,
                                               __uuidof(ID3D12Resource),
                                               resource.put_void())))
        return {};

    return resource;
}
} // namespace

// Off-screen GPU snapshot for View::renderToImage.
Graphics::Image GPUView::renderNativeContent(float scale)
{
    auto bounds = getLocalBounds();
    auto pixelWidth = static_cast<UINT>(std::lround(bounds.w * scale));
    auto pixelHeight = static_cast<UINT>(std::lround(bounds.h * scale));

    if (pixelWidth == 0 || pixelHeight == 0)
        return {};

    auto& context = getD3D12Context();
    if (!context.isValid())
        return {};

    auto* device = context.getDevice();
    auto samples = static_cast<UINT>(impl->sampleCount);
    auto useMsaa = samples > 1;
    auto useDepth = impl->depthEnabled;

    constexpr auto colorFormat = DXGI_FORMAT_B8G8R8A8_UNORM;

    winrt::com_ptr<ID3D12DescriptorHeap> rtvHeap;
    D3D12_DESCRIPTOR_HEAP_DESC rtvHeapDesc = {};
    rtvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    rtvHeapDesc.NumDescriptors = 2;
    if (FAILED(device->CreateDescriptorHeap(
            &rtvHeapDesc, __uuidof(ID3D12DescriptorHeap), rtvHeap.put_void())))
        return {};

    auto rtvSize =
        device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    auto colorRtv = rtvHeap->GetCPUDescriptorHandleForHeapStart();
    auto msaaRtv = colorRtv;
    msaaRtv.ptr += rtvSize;

    // The Frame destructor keys off the same distinction.
    auto colorInitial = useMsaa ? D3D12_RESOURCE_STATE_RESOLVE_DEST
                                : D3D12_RESOURCE_STATE_RENDER_TARGET;
    auto colorTexture = makeSnapshotTexture(device,
                                            pixelWidth,
                                            pixelHeight,
                                            colorFormat,
                                            1,
                                            D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET,
                                            colorInitial,
                                            nullptr);
    if (!colorTexture)
        return {};

    device->CreateRenderTargetView(colorTexture.get(), nullptr, colorRtv);

    winrt::com_ptr<ID3D12Resource> msaaTexture;
    if (useMsaa)
    {
        msaaTexture = makeSnapshotTexture(device,
                                          pixelWidth,
                                          pixelHeight,
                                          colorFormat,
                                          samples,
                                          D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET,
                                          D3D12_RESOURCE_STATE_RENDER_TARGET,
                                          nullptr);
        if (!msaaTexture)
            return {};

        device->CreateRenderTargetView(msaaTexture.get(), nullptr, msaaRtv);
    }

    winrt::com_ptr<ID3D12DescriptorHeap> dsvHeap;
    winrt::com_ptr<ID3D12Resource> depthTexture;
    D3D12_CPU_DESCRIPTOR_HANDLE depthDsv = {};

    if (useDepth)
    {
        D3D12_DESCRIPTOR_HEAP_DESC dsvHeapDesc = {};
        dsvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
        dsvHeapDesc.NumDescriptors = 1;

        if (SUCCEEDED(device->CreateDescriptorHeap(
                &dsvHeapDesc, __uuidof(ID3D12DescriptorHeap), dsvHeap.put_void())))
        {
            D3D12_CLEAR_VALUE clearValue = {};
            clearValue.Format = DXGI_FORMAT_D32_FLOAT;
            clearValue.DepthStencil.Depth = 1.0f;

            depthTexture =
                makeSnapshotTexture(device,
                                    pixelWidth,
                                    pixelHeight,
                                    DXGI_FORMAT_D32_FLOAT,
                                    useMsaa ? samples : 1,
                                    D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL,
                                    D3D12_RESOURCE_STATE_DEPTH_WRITE,
                                    &clearValue);

            if (depthTexture)
            {
                depthDsv = dsvHeap->GetCPUDescriptorHandleForHeapStart();
                device->CreateDepthStencilView(
                    depthTexture.get(), nullptr, depthDsv);
            }
        }
    }

    D3D12Drawable colorTarget = {};
    colorTarget.backBuffer = colorTexture.get();
    colorTarget.backBufferView = colorRtv;
    colorTarget.width = pixelWidth;
    colorTarget.height = pixelHeight;

    D3D12MsaaTarget msaaTarget = {};
    if (useMsaa)
    {
        msaaTarget.texture = msaaTexture.get();
        msaaTarget.view = msaaRtv;
        msaaTarget.format = colorFormat;
    }

    D3D12DepthTarget depthTarget = {};
    if (depthTexture)
        depthTarget.view = depthDsv;

    {
        OffscreenTarget target = {};
        target.colorTexture = &colorTarget;
        target.msaaTexture = useMsaa ? &msaaTarget : nullptr;
        target.depthTexture = depthTexture ? &depthTarget : nullptr;

        auto frame = Frame(Device::shared(), target);
        render(frame);
    }
    // The Frame destructor left the colour texture in COPY_SOURCE and ran the
    // GPU to completion.
    auto colorDesc = colorTexture->GetDesc();
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint = {};
    UINT64 totalBytes = 0;
    device->GetCopyableFootprints(
        &colorDesc, 0, 1, 0, &footprint, nullptr, nullptr, &totalBytes);

    D3D12_HEAP_PROPERTIES readbackHeap = {};
    readbackHeap.Type = D3D12_HEAP_TYPE_READBACK;

    D3D12_RESOURCE_DESC bufferDesc = {};
    bufferDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bufferDesc.Width = totalBytes;
    bufferDesc.Height = 1;
    bufferDesc.DepthOrArraySize = 1;
    bufferDesc.MipLevels = 1;
    bufferDesc.Format = DXGI_FORMAT_UNKNOWN;
    bufferDesc.SampleDesc.Count = 1;
    bufferDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    winrt::com_ptr<ID3D12Resource> readback;
    if (FAILED(device->CreateCommittedResource(&readbackHeap,
                                               D3D12_HEAP_FLAG_NONE,
                                               &bufferDesc,
                                               D3D12_RESOURCE_STATE_COPY_DEST,
                                               nullptr,
                                               __uuidof(ID3D12Resource),
                                               readback.put_void())))
        return {};

    auto* commands = context.acquire();

    D3D12_TEXTURE_COPY_LOCATION dstLocation = {};
    dstLocation.pResource = readback.get();
    dstLocation.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dstLocation.PlacedFootprint = footprint;

    D3D12_TEXTURE_COPY_LOCATION srcLocation = {};
    srcLocation.pResource = colorTexture.get();
    srcLocation.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    srcLocation.SubresourceIndex = 0;

    commands->list->CopyTextureRegion(&dstLocation, 0, 0, 0, &srcLocation, nullptr);
    context.submit(commands);
    context.waitIdle();

    void* mappedPtr = nullptr;
    D3D12_RANGE readRange = {0, static_cast<SIZE_T>(totalBytes)};
    if (FAILED(readback->Map(0, &readRange, &mappedPtr)) || mappedPtr == nullptr)
        return {};

    auto image = Graphics::Image {};
    auto* dst = image.prepareForOverwrite(static_cast<int>(pixelWidth),
                                          static_cast<int>(pixelHeight));

    if (dst == nullptr)
    {
        readback->Unmap(0, nullptr);
        return {};
    }

    auto rowPitch = footprint.Footprint.RowPitch;
    auto* base = static_cast<const std::uint8_t*>(mappedPtr);

    // Premultiplied BGRA8, as the compositor treats the swapchain, to the
    // straight RGBA Image holds, row by row past the read-back pitch alignment.
    for (auto y = UINT {0}; y < pixelHeight; ++y)
    {
        auto* srcRow = base + static_cast<std::size_t>(y) * rowPitch;

        for (auto x = UINT {0}; x < pixelWidth; ++x)
        {
            auto* src = srcRow + static_cast<std::size_t>(x) * 4;
            auto* out = dst + (static_cast<std::size_t>(y) * pixelWidth + x) * 4;

            auto b = src[0];
            auto g = src[1];
            auto r = src[2];
            auto a = src[3];

            if (a == 0)
            {
                out[0] = out[1] = out[2] = out[3] = 0;
                continue;
            }

            auto straight = [&](std::uint8_t c) -> std::uint8_t
            {
                return static_cast<std::uint8_t>(
                    (std::min) (255, (c * 255 + a / 2) / a));
            };

            out[0] = straight(r);
            out[1] = straight(g);
            out[2] = straight(b);
            out[3] = a;
        }
    }

    readback->Unmap(0, nullptr);
    return image;
}

bool GPUView::renderNativeContentToTarget(void*, float)
{
    // No zero-copy capture on D3D12 yet; callers fall back to
    // renderNativeContent or the screen-capture tier.
    return false;
}
} // namespace eacp::GPU

#include "GPUView.h"
#include <eacp/Graphics/DComp-Windows.h>

#include "../Device/Device.h"
#include "../Frame/Frame.h"
#include "../Windows/D3D12Types.h"

#include <eacp/Graphics/Image/Image.h>

#include <algorithm>
#include <cmath>
#include <unordered_set>

namespace eacp::GPU
{
namespace
{
// A buffer for each frame the swapchain may have in flight — the one being
// shown, the one queued, and the one being drawn. The per-buffer fence in
// render() stops the CPU from reusing a buffer the GPU still reads, and
// DXGI is never asked to hold more frames than there are buffers to hold them.
// See GPUView::setFramesInFlight for what the frames themselves cost.
constexpr int maxFramesInFlight = 3;
constexpr UINT bufferCount = maxFramesInFlight;

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

        // The last frames may still reference the back buffers and targets
        // about to be released (waitIdle also settles the async submit).
        getD3D12Context().waitIdle();

        if (spriteVisual)
            if (auto* container =
                    static_cast<IDCompositionVisual2*>(view.getNativeLayer()))
                container->RemoveVisual(spriteVisual.Get());

        Graphics::commitComposition();
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
        if (frameLatencyWaitable != nullptr)
        {
            CloseHandle(frameLatencyWaitable);
            frameLatencyWaitable = nullptr;
        }

        swapChain = nullptr;
        frameFences = {};

        if (spriteVisual)
            spriteVisual->SetContent(nullptr);

        // The DComp device is replaced along with the rendering device, so
        // re-acquire it and rebuild the visual before the swapchain reattaches.
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

        // The corrected transform must reach the compositor NOW: presents
        // flow to the screen without commits, so without this the screen
        // keeps a drag's last stretch applied to the rebuilt buffers — wrong
        // content until some unrelated repaint happens to commit.
        Graphics::commitComposition();
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

        // A waitable swapchain is what makes the present queue's depth ours to
        // choose. Without it DXGI queues up to three frames of its own accord,
        // and the picture on screen can be three refreshes behind the hand that
        // moved. See GPUView::setFramesInFlight.
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

    // DComp takes a swapchain as visual content directly — no interop surface and
    // no surface brush, which is what WinRT needed CreateCompositionSurfaceForSwap
    // Chain + CompositionStretch::Fill for. The swapchain is already sized in
    // physical pixels, so the visual counter-scales by 1/dpiScale to cancel the
    // root's DPI transform (see NativeLayer-Windows.h).
    void attachSwapChainToVisual()
    {
        if (!spriteVisual || !swapChain)
            return;

        spriteVisual->SetContent(swapChain.get());
        applyContentScale();
        Graphics::commitComposition();
    }

    // Maps the swapchain's pixels onto the view's current bounds. At rest the
    // two agree and this is the plain 1/dpi counter-scale; mid-resize (see the
    // stretch note in render()) it scales the old buffers to the new bounds so
    // the picture tracks the drag at full frame rate.
    void applyContentScale()
    {
        auto scale = dpiScale();

        if (!spriteVisual || scale <= 0.f || width == 0 || height == 0)
            return;

        auto bounds = view.getLocalBounds();
        auto targetWidth = bounds.w * scale;
        auto targetHeight = bounds.h * scale;

        spriteVisual->SetTransform(
            D2D1::Matrix3x2F::Scale(targetWidth / (float) width / scale,
                                    targetHeight / (float) height / scale));
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
        // frame, and ResizeBuffers requires every outstanding reference gone —
        // including a present the context's worker is still inside (waitIdle
        // settles that too).
        getD3D12Context().waitIdle();

        for (auto& buffer: backBuffers)
            buffer = nullptr;

        frameFences = {};

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

    // How many frames DXGI may hold, presented but not yet shown. A waitable
    // swapchain defaults to one, which is the least latency there is but leaves
    // the GPU waiting on the CPU between frames; Microsoft's own guidance is
    // that two is what keeps the two working in parallel. It is the same number
    // Metal is given, so the backends queue alike.
    void applyFrameLatency()
    {
        if (swapChain)
            swapChain->SetMaximumFrameLatency(static_cast<UINT>(framesInFlight));
    }

    // Applies and commits the stretch for the size change being processed
    // right now, inside the same WM_SIZE that changed the bounds. During a
    // size/move drag this is the ONLY thing that runs per size change: the
    // frozen last frame scales with the window through this transform, so
    // the picture tracks the edge with nothing else contending for the
    // thread the drag loop runs on.
    void applyImmediateStretch()
    {
        if (!swapChain)
            return;

        applyContentScale();
        Graphics::commitComposition();
    }

    void render()
    {
        auto& context = getD3D12Context();

        // Deferred from resized(): a live resize delivers WM_SIZE faster than
        // frames, and a swapchain rebuild drains the GPU (20-50 ms at 1440p) —
        // rebuilding per message made dragging a window edge unusable. While
        // the size is still changing, the old buffers stretch to the new
        // bounds through the visual transform instead, so the picture tracks
        // the drag at full frame rate; the rebuild runs once the drag pauses
        // (or at a staleness bound, so a marathon drag cannot drift
        // arbitrarily far from native resolution).
        // Deferred from resized(): the rebuild drains the GPU (20-50 ms at
        // 1440p), so it never runs inside a size/move drag — mid-drag frames
        // render into the old buffers, which the transform resized() commits
        // stretches onto the moving bounds, and the rebuild lands on the
        // first frame after the drag ends. Outside a drag (maximise,
        // restore, programmatic sizes) it runs at most once per frame, at
        // the latest size.
        auto insideDrag = Graphics::isInsideSizeMoveLoop();
        auto resizedThisFrame = false;

        if (sizeDirty && !insideDrag)
        {
            sizeDirty = false;
            resizedThisFrame = true;
            updateSize();
        }

        if (!swapChain || !context.isValid() || width == 0 || height == 0)
            return;

        // One frame in the async pipeline at a time: skipping while the
        // context's worker still holds the previous frame keeps the
        // back-buffer index read below valid, and coalesces frames to
        // whatever rate the compositor actually absorbs.
        if (context.isAsyncSubmitPending())
            return;

        // Blocks until the swapchain is ready for another frame, so the CPU
        // runs no further ahead of the display than it was told it may. Two
        // exceptions:
        //
        //  - Not after a rebuild: ResizeBuffers empties the present queue and
        //    can leave the waitable unsignalled until a present retires, so
        //    waiting would show the freshly cleared buffers for the full
        //    timeout. With nothing queued there is nothing to pace against.
        //
        //  - Never BLOCKING inside a size/move drag: this tick runs on the
        //    thread the OS drag loop is tracking the mouse with, and playback
        //    is not worth the edge lagging the hand. No free slot right now
        //    means the compositor is behind — skip the frame instead.
        if (frameLatencyWaitable != nullptr && !resizedThisFrame)
        {
            auto wait = WaitForSingleObjectEx(
                frameLatencyWaitable, insideDrag ? 0 : 1000, TRUE);

            if (insideDrag && wait != WAIT_OBJECT_0)
                return;
        }

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
        drawable.deferPresent = true;

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

        // The Frame destructor hands execute/signal/present to the context's
        // worker (deferPresent above); the fence value is claimed before it
        // returns, so lastSubmitted() below is this frame's.
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

    // GetMessage hands out posted messages before hardware input, and a
    // continuous render stream arrives as posted messages — so whenever a few
    // frames run long, the mouse can sit in the queue for the whole busy
    // stretch (measured at ~800 ms during a hover). Each tick therefore
    // dispatches the queued input itself before the next frame. Inside the
    // OS's own modal loops it stands down: those loops own the input, and
    // removing a drag's mouse moves from under one breaks the drag itself.
    static void dispatchPendingInput()
    {
        if (Graphics::isInsideNativeModalLoop())
            return;

        auto msg = MSG {};
        auto budget = 32; // bound what an input flood can pull into one tick

        while (
            budget-- > 0
            && (PeekMessageW(&msg, nullptr, WM_MOUSEFIRST, WM_MOUSELAST, PM_REMOVE)
                || PeekMessageW(&msg, nullptr, WM_KEYFIRST, WM_KEYLAST, PM_REMOVE)))
        {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }

    // The tick advances animation state and renders on the spot. It must NOT
    // go through repaint()/WM_PAINT: paint messages are the queue's lowest
    // priority, delivered only when nothing else is pending, so a mouse-move
    // storm (hovering a video wall) or the modal size/move loop starves an
    // invalidation-driven render into visible frame drops. The tick arrives
    // as a posted message — dispatched ahead of input and inside foreign
    // modal loops — so rendering here holds the compositor's cadence exactly
    // when the queue is busiest. Pacing is preserved by the swapchain's
    // frame-latency waitable in render(); one tick renders at most one frame.
    //
    // Input is pumped last: a handler may close the window and destroy this
    // view, so nothing may touch `this` afterwards.
    void startContinuous()
    {
        if (displayLink == nullptr)
        {
            auto func = [this](Threads::FrameTime time)
            {
                view.update(time);
                view.renderNow();
                dispatchPendingInput();
            };

            displayLink.create(func);
        }
    }

    void stopContinuous() { displayLink = nullptr; }

    GPUView& view;
    int sampleCount = 4;

    // Two by default, so a hand is answered a refresh sooner than DXGI's own
    // three would allow. See GPUView::setFramesInFlight.
    int framesInFlight = 2;
    HANDLE frameLatencyWaitable = nullptr;

    bool continuous = false;
    bool depthEnabled = false;
    bool sizeDirty = false;
    UINT width = 0;
    UINT height = 0;

    IDCompositionDesktopDevice* compositionDevice = nullptr;
    Microsoft::WRL::ComPtr<IDCompositionVisual2> spriteVisual;
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

    // The swapchain rebuild is deferred to the next render — see render() —
    // but the stretch that keeps the picture tracking the drag is applied and
    // committed immediately, inside this very WM_SIZE.
    impl->sizeDirty = true;
    impl->applyImmediateStretch();
    repaint();
}

void GPUView::backingScaleChanged()
{
    Graphics::View::backingScaleChanged();

    // Resize the swapchain to the new scale (same logical bounds, different
    // pixel count), then redraw: the presented frame was built for the old one.
    impl->sizeDirty = true;
    onBackingScaleChanged(backingScale());
    repaint();
}

float GPUView::backingScale() const
{
    return Native::dpiScale();
}

void GPUView::paint(Graphics::Context& context)
{
    // A snapshot captures GPU content via renderNativeContent (off-screen); the
    // live renderNow() here would present an on-screen frame as a side effect.
    if (context.isSnapshot())
        return;

    // A continuous view presents from its display tick; rendering here too
    // would present twice per refresh and leave every other render blocked in
    // the swapchain's latency wait (WM_SIZE invalidates while resizing, so a
    // live resize hits this constantly).
    if (impl->continuous)
        return;

    renderNow();
}

void GPUView::renderNow()
{
    impl->render();
}

namespace
{
// A committed default-heap texture for the off-screen snapshot, mirroring
// GPUView-Apple.mm's makeTarget: a colour/MSAA render target or a depth target.
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

// Off-screen GPU snapshot for View::renderToImage, mirroring GPUView-Apple.mm:
// render() draws into an app-owned colour texture (resolving from an MSAA target
// when multisampling) through an off-screen Frame that waits instead of
// presenting, then the colour texture is copied to a read-back buffer and
// swizzled from BGRA to the straight RGBA an Image holds.
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

    // One RTV heap (colour + optional MSAA target), one DSV heap.
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

    // The colour texture is single-sampled: the render target when not
    // multisampling (RENDER_TARGET), or the resolve destination when the pass
    // renders into the MSAA target (RESOLVE_DEST). The Frame destructor keys off
    // the same distinction.
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

    // BGRA8 premultiplied (how the compositor treats the swapchain) -> straight
    // RGBA (what Image holds), row by row past the 256-byte read-back pitch
    // alignment.
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
    // Zero-copy video capture (render straight into a shared D3D/DXGI surface)
    // is not wired on the D3D12 backend yet; callers fall back to the read-back
    // path (renderNativeContent) or the screen-capture tier.
    return false;
}
} // namespace eacp::GPU

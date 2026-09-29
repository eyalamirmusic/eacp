#include "GPUView.h"

#include "../Device/Device.h"
#include "../Frame/Frame.h"
#include "../WebGPU/WebGPUTypes.h"

#include <eacp/Graphics/Helpers/DisplayLink.h>
#include <eacp/Graphics/View/View-Linux.h>

#include <algorithm>
#include <chrono>
#include <cmath>

namespace eacp::GPU
{
namespace
{
// What RenderPipelineDescriptor::colorFormat defaults to, and one every canvas
// takes whatever it prefers: a pipeline built for the default draws into it.
constexpr auto drawableFormat = WGPUTextureFormat_BGRA8Unorm;
} // namespace

struct GPUView::Native
{
    explicit Native(GPUView& viewToUse)
        : view(viewToUse)
        , record(Graphics::requestViewSurface(viewToUse))
    {
        record.onAvailable = [this] { surfaceAvailable(); };
        record.onLost = [this] { surfaceLost(); };
        record.onResized = [this] { surfaceResized(); };
        record.onRepaint = [this] { render(); };
        record.onFrameDone = [this] { frameCallbackArrived(); };
    }

    ~Native()
    {
        // The record outlives this: onLost fires from ~View, after the Pimpl
        // has gone, so the hooks must stop pointing here first.
        record.onAvailable = [] {};
        record.onLost = [] {};
        record.onResized = [] {};
        record.onRepaint = [] {};
        record.onFrameDone = [] {};

        stopContinuous();
        destroySurface();
    }

    void surfaceAvailable()
    {
        if (!createSurface())
            return;

        surfaceStale = true;

        if (continuous)
            continuousTick(true);
        else
            render();
    }

    void surfaceLost() { destroySurface(); }

    void surfaceResized()
    {
        surfaceStale = true;
        view.repaint();
    }

    void frameCallbackArrived()
    {
        if (continuous)
            continuousTick(false);
    }

    float surfaceScale() const
    {
        if (record.handle.isValid() && record.scale > 0.f)
            return record.scale;

        return Graphics::linuxDefaultBackingScale;
    }

    void notifyScaleChange()
    {
        if (!record.handle.isValid())
            return;

        const auto scale = surfaceScale();
        const auto changed = backingScale > 0.f && scale != backingScale;
        backingScale = scale;

        if (changed)
            view.onBackingScaleChanged(scale);
    }

    void startContinuous()
    {
        stampedTick = Threads::DisplayLink::timedTick(
            [this](Threads::FrameTime time)
            {
                view.update(time);
                render();
            });

        pacingStarted = false;
        continuousTick(true);
    }

    void stopContinuous()
    {
        stampedTick = [] {};
        pacingStarted = false;
    }

    // The half-tick grace keeps 60 on a 120 Hz display from missing the
    // interval by float dust and running at 40.
    bool tickIsDue()
    {
        using Clock = std::chrono::steady_clock;

        if (maxFps <= 0)
        {
            pacingStarted = false;
            return true;
        }

        const auto interval = 1.0 / static_cast<double>(maxFps);
        const auto now = Clock::now();

        if (!pacingStarted)
        {
            pacingStarted = true;
            lastTick = now;
            accumulated = interval;
        }

        const auto period = std::chrono::duration<double>(now - lastTick).count();
        lastTick = now;
        accumulated += period;

        if (accumulated + period * 0.5 < interval)
            return false;

        accumulated = std::min(accumulated - interval, interval * 0.5);
        return true;
    }

    // Each animation frame asks for the next, whether or not it drew.
    void continuousTick(bool force)
    {
        if (!continuous || surface == nullptr)
            return;

        if (!force && !tickIsDue())
        {
            record.requestFrameCallback();
            return;
        }

        stampedTick();
    }

    bool createSurface()
    {
        auto& shared = getWebGPUShared();

        if (!shared.isValid() || record.handle.selector == nullptr)
            return false;

        if (surface != nullptr)
            return true;

        auto canvas = WGPU_EMSCRIPTEN_SURFACE_SOURCE_CANVAS_HTML_SELECTOR_INIT;
        canvas.selector = toWebString(record.handle.selector);

        auto descriptor = WGPU_SURFACE_DESCRIPTOR_INIT;
        descriptor.nextInChain = &canvas.chain;

        surface = wgpuInstanceCreateSurface(shared.getInstance(), &descriptor);

        return surface != nullptr;
    }

    void destroySurface()
    {
        releaseWebCompanions(companions);
        companions = {};

        if (surface == nullptr)
            return;

        if (configured)
            wgpuSurfaceUnconfigure(surface);

        wgpuSurfaceRelease(surface);
        surface = nullptr;
        configured = false;
    }

    bool configure()
    {
        surfaceStale = false;
        companionsStale = false;

        releaseWebCompanions(companions);
        companions = {};

        if (record.pixelWidth <= 0 || record.pixelHeight <= 0)
            return false;

        auto configuration = WGPU_SURFACE_CONFIGURATION_INIT;
        configuration.device = getWebGPUShared().getDevice();
        configuration.format = drawableFormat;
        configuration.usage = WGPUTextureUsage_RenderAttachment;
        configuration.width = static_cast<std::uint32_t>(record.pixelWidth);
        configuration.height = static_cast<std::uint32_t>(record.pixelHeight);
        configuration.alphaMode = WGPUCompositeAlphaMode_Opaque;
        configuration.presentMode = WGPUPresentMode_Fifo;

        wgpuSurfaceConfigure(surface, &configuration);
        configured = true;

        return createCompanions();
    }

    bool createCompanions()
    {
        companionsStale = false;
        releaseWebCompanions(companions);
        companions = {};

        auto& gpu = Device::shared();

        companions.format = drawableFormat;
        companions.width = record.pixelWidth;
        companions.height = record.pixelHeight;
        companions.sampleCount =
            sampleCount > 1 && gpu.supportsSampleCount(sampleCount) ? sampleCount
                                                                    : 1;

        if (companions.sampleCount > 1 && !createWebMultisampleCompanion(companions))
            return false;

        if (depthEnabled || stencilEnabled)
            createWebDepthCompanion(companions, stencilEnabled, false);

        return true;
    }

    bool readyToRender()
    {
        if (!record.handle.isValid() || surface == nullptr)
            return false;

        if (!Device::shared().isValid())
            return false;

        if (surfaceStale && !configure())
            return false;

        if (companionsStale && !createCompanions())
            return false;

        return configured;
    }

    // Not re-entrant: view.render() may call repaint().
    void render()
    {
        if (rendering || !readyToRender())
            return;

        rendering = true;
        renderOneFrame();
        rendering = false;
    }

    void renderOneFrame()
    {
        auto current = WGPU_SURFACE_TEXTURE_INIT;
        wgpuSurfaceGetCurrentTexture(surface, &current);

        if (current.status != WGPUSurfaceGetCurrentTextureStatus_SuccessOptimal
            && current.status
                   != WGPUSurfaceGetCurrentTextureStatus_SuccessSuboptimal)
        {
            if (current.texture != nullptr)
                wgpuTextureRelease(current.texture);

            surfaceStale = true;
            return;
        }

        auto viewDescriptor = WGPU_TEXTURE_VIEW_DESCRIPTOR_INIT;
        auto textureView = wgpuTextureCreateView(current.texture, &viewDescriptor);

        // The companions lent for this frame; `companions` keeps ownership.
        auto target = companions;
        target.texture = current.texture;
        target.attachmentView = textureView;

        // The first one rides on this frame; after that each tick asks again.
        record.requestFrameCallback();

        {
            auto frame =
                Frame(Device::shared(), &target, nullptr, nullptr, surfaceScale());
            view.render(frame);
        }

        wgpuTextureViewRelease(textureView);
        wgpuTextureRelease(current.texture);
    }

    GPUView& view;
    Graphics::ViewSurface& record;

    int sampleCount = 1;
    int maxFps = 0;
    int framesInFlight = 2;
    bool depthEnabled = false;
    bool stencilEnabled = false;
    bool continuous = false;

    WGPUSurface surface = nullptr;
    bool configured = false;
    WebTextureData companions;

    bool surfaceStale = false;
    bool companionsStale = false;
    bool rendering = false;

    float backingScale = 0.f;

    Callback stampedTick = [] {};

    std::chrono::steady_clock::time_point lastTick;
    double accumulated = 0.0;
    bool pacingStarted = false;
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
    impl->companionsStale = true;
}

void GPUView::setDepth(bool enabled)
{
    impl->depthEnabled = enabled;

    // The stencil plane lives in the depth attachment, so it goes with it.
    if (!enabled)
        impl->stencilEnabled = false;

    impl->companionsStale = true;
}

bool GPUView::hasDepth() const
{
    return impl->depthEnabled;
}

void GPUView::setStencil(bool enabled)
{
    impl->stencilEnabled = enabled;

    if (enabled)
        impl->depthEnabled = true;

    impl->companionsStale = true;
}

bool GPUView::hasStencil() const
{
    return impl->stencilEnabled;
}

void GPUView::setContinuous(bool continuous)
{
    if (impl->continuous == continuous)
        return;

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

void GPUView::setMaxFps(int fps)
{
    impl->maxFps = fps;
    impl->pacingStarted = false;
}

int GPUView::maxFps() const
{
    return impl->maxFps;
}

// The browser decides how many frames are in flight; the count is kept so a
// caller reads back what it set.
void GPUView::setFramesInFlight(int count)
{
    impl->framesInFlight = count < 1 ? 1 : count;
}

int GPUView::framesInFlight() const
{
    return impl->framesInFlight;
}

void GPUView::resizeStarted()
{
    impl->notifyScaleChange();
}

void GPUView::resizeFinished() {}

float GPUView::backingScale() const
{
    if (renderScale > 0.f)
        return renderScale;

    return impl->surfaceScale();
}

void GPUView::paint(Graphics::Context&)
{
    // Never called on the web: there is no 2D Context.
}

void GPUView::renderNow()
{
    impl->render();
}

// A snapshot is a render into a texture and a read back, and the read cannot
// wait on the main thread.
Graphics::Image GPUView::renderNativeContent(float)
{
    reportWebUnsupported("GPUView::renderNativeContent");
    return {};
}

bool GPUView::renderNativeContentToTarget(void*, float)
{
    return false;
}
} // namespace eacp::GPU

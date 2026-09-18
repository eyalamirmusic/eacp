#include "GPUView.h"

#include "../Device/Device.h"
#include "../Frame/Frame.h"
#include "../Linux/GPUBackend-Linux.h"
#include "../Texture/Texture.h"

#include <eacp/Graphics/Helpers/DisplayLink.h>
#include <eacp/Graphics/View/View-Linux.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>

// The half of a presenting view neither graphics API owns: the surface hooks,
// the continuous tick and its divider, and the off-screen render that touches
// no drawable at all. The swapchain under it is the backend's - see
// GPUViewBackend.
namespace eacp::GPU
{
struct GPUView::Native
{
    explicit Native(GPUView& viewToUse)
        : view(viewToUse)
        , record(Graphics::requestViewSurface(viewToUse))
        , backend(getDeviceBackend(Device::shared()).makeGPUView(viewToUse, record))
    {
        // Asking for the record is what marks this view as one that presents.
        record.onAvailable = [this] { surfaceAvailable(); };
        record.onLost = [this] { surfaceLost(); };
        record.onResized = [this] { surfaceResized(); };
        record.onRepaint = [this] { render(); };
        record.onFrameDone = [this] { frameCallbackArrived(); };

        // A device that goes has nothing left to pace.
        backend->onDeviceLost = [this] { stopContinuous(); };
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
    }

    void surfaceAvailable()
    {
        if (!backend->surfaceAvailable())
            return;

        // No frame callback arrives until something has been presented.
        if (continuous)
            continuousTick(true);
        else
            render();
    }

    void surfaceLost() { backend->surfaceLost(); }

    void surfaceResized()
    {
        backend->surfaceResized();

        // A compositor that resized a surface is not obliged to repaint it.
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

    // A scale change tells itself apart from a resize by the surface's own
    // scale. Nothing is recorded before a surface carries a real one, so the
    // first that arrives is the initial scale rather than a change.
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

    // The half-tick grace keeps 60 on a 120 Hz compositor from missing the
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

    void continuousTick(bool force)
    {
        if (!continuous)
            return;

        // Dropped here, not at render(): a frame never drawn must not have
        // advanced update().
        if (!backend->isPresenting())
            return;

        // A tick that presents nothing earns no frame callback of its own, so
        // the request is made here; requestFrameCallback commits for it.
        if (!force && !tickIsDue())
        {
            record.requestFrameCallback();
            return;
        }

        stampedTick();
    }

    // Not re-entrant: view.render() may call repaint(), and a second frame would
    // acquire against a semaphore the first is still waiting on.
    void render()
    {
        if (rendering || !backend->readyToRender())
            return;

        rendering = true;
        backend->renderOneFrame(surfaceScale());
        rendering = false;
    }

    GPUView& view;
    Graphics::ViewSurface& record;

    std::unique_ptr<GPUViewBackend> backend;

    int sampleCount = 1;
    int maxFps = 0;
    int framesInFlight = 2;
    bool depthEnabled = false;
    bool stencilEnabled = false;
    bool continuous = false;
    bool rendering = false;

    // Zero until the surface reports one, which is how the initial scale is told
    // apart from a change.
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
    impl->backend->setSampleCount(count);
}

void GPUView::setDepth(bool enabled)
{
    impl->depthEnabled = enabled;

    // The stencil plane lives in the depth attachment, so it goes with it.
    if (!enabled)
        impl->stencilEnabled = false;

    impl->backend->setDepth(impl->depthEnabled, impl->stencilEnabled);
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

    impl->backend->setDepth(impl->depthEnabled, impl->stencilEnabled);
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

    // The pacing restarts at the new rate rather than carrying the old one's
    // accumulated remainder into it.
    impl->pacingStarted = false;
}

int GPUView::maxFps() const
{
    return impl->maxFps;
}

void GPUView::setFramesInFlight(int count)
{
    impl->framesInFlight = count < 1 ? 1 : count;
    impl->backend->setFramesInFlight(impl->framesInFlight);
}

int GPUView::framesInFlight() const
{
    return impl->framesInFlight;
}

// No drawable to resize here: the swapchain is rebuilt from the surface's own
// configure, so all this owes either event is the scale notification.
void GPUView::resizeStarted()
{
    impl->notifyScaleChange();
}

// Nothing to order after the subclass: the swapchain is rebuilt from that
// configure and the frame drawn from a frame callback, both after every
// override has run.
void GPUView::resizeFinished() {}

float GPUView::backingScale() const
{
    if (renderScale > 0.f)
        return renderScale;

    return impl->surfaceScale();
}

void GPUView::paint(Graphics::Context&)
{
    // Never called on Linux: there is no 2D Context.
}

void GPUView::renderNow()
{
    impl->render();
}

// Touches no swapchain, so this is also what runs headless.
Graphics::Image GPUView::renderNativeContent(float scale)
{
    const auto bounds = getLocalBounds();
    const auto pixelWidth = static_cast<int>(std::lround(bounds.w * scale));
    const auto pixelHeight = static_cast<int>(std::lround(bounds.h * scale));

    if (pixelWidth <= 0 || pixelHeight <= 0)
        return {};

    auto& device = Device::shared();

    if (!device.isValid())
        return {};

    auto descriptor = TextureDescriptor {};
    descriptor.width = pixelWidth;
    descriptor.height = pixelHeight;

    descriptor.format = TextureFormat::BGRA8Unorm;
    descriptor.renderTarget = true;

    descriptor.sampleCount =
        device.supportsSampleCount(impl->sampleCount) ? impl->sampleCount : 1;

    descriptor.depth = impl->depthEnabled;
    descriptor.stencil = impl->stencilEnabled;

    auto texture = device.makeTexture(descriptor);

    if (!texture.isValid())
        return {};

    {
        auto target = OffscreenTarget {};
        target.colorTexture = texture.nativeTexture();

        auto frame = Frame(device, target, scale);

        renderScale = scale;
        render(frame);
        renderScale = 0.f;
    }
    // The Frame destructor submitted and waited, so the texture can be read.

    const auto pixelCount =
        static_cast<std::size_t>(pixelWidth) * static_cast<std::size_t>(pixelHeight);

    auto bgra = Vector<std::uint8_t>(static_cast<int>(pixelCount * 4));

    texture.read(bgra.data());

    auto image = Graphics::Image {};
    auto* dst = image.prepareForOverwrite(pixelWidth, pixelHeight);

    if (dst == nullptr)
        return {};

    for (auto pixel = std::size_t {0}; pixel < pixelCount; ++pixel)
    {
        const auto* src = bgra.data() + pixel * 4;
        auto* out = dst + pixel * 4;

        const auto b = src[0];
        const auto g = src[1];
        const auto r = src[2];
        const auto a = src[3];

        if (a == 0)
        {
            out[0] = out[1] = out[2] = out[3] = 0;
            continue;
        }

        const auto straight = [a](std::uint8_t channel)
        {
            return static_cast<std::uint8_t>(
                std::min(255, (channel * 255 + a / 2) / a));
        };

        out[0] = straight(r);
        out[1] = straight(g);
        out[2] = straight(b);
        out[3] = a;
    }

    return image;
}

bool GPUView::renderNativeContentToTarget(void*, float)
{
    return false;
}
} // namespace eacp::GPU

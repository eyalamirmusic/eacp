#include "GPUComponent.h"

#include "../Host/ComponentHost.h"

#include <atomic>

namespace eacp::UI
{
GPUComponent::GPUComponent()
    : layer(*this)
{
    layer.frameComponent = this;
    layer.opaque = true;

    setInterceptsMouseClicks(true);
}

GPUComponent::~GPUComponent()
{
    stopAnimating();
}

void GPUComponent::render(GPU::Frame&) {}

void GPUComponent::update(Threads::FrameTime) {}

int GPUComponent::sampleCount() const
{
    return layer.getSampleCount();
}

void GPUComponent::setSampleCount(int count)
{
    layer.setSampleCount(count);
}

void GPUComponent::setDepth(bool enabled)
{
    layer.setDepth(enabled);

    if (!enabled)
        layer.setStencil(false);
}

bool GPUComponent::hasDepth() const
{
    return layer.hasDepth() || layer.hasStencil();
}

void GPUComponent::setStencil(bool enabled)
{
    layer.setStencil(enabled);

    if (enabled)
        layer.setDepth(true);
}

bool GPUComponent::hasStencil() const
{
    return layer.hasStencil();
}

void GPUComponent::setTransparent(bool shouldBeTransparent)
{
    if (layer.opaque != shouldBeTransparent)
        return;

    layer.opaque = !shouldBeTransparent;
    invalidateHost();
}

bool GPUComponent::isTransparent() const
{
    return !layer.opaque;
}

void GPUComponent::setContinuous(bool shouldBeContinuous)
{
    if (continuous == shouldBeContinuous)
        return;

    continuous = shouldBeContinuous;

    if (continuous)
        resumeIfContinuous();
    else
        stopAnimating();
}

bool GPUComponent::isContinuous() const
{
    return continuous;
}

void GPUComponent::setMaxFps(int fps)
{
    pacer.setMaxFps(fps);
}

int GPUComponent::maxFps() const
{
    return pacer.maxFps();
}

void GPUComponent::setOpacity(float newOpacity)
{
    layer.setOpacity(newOpacity);
}

float GPUComponent::getOpacity() const
{
    return layer.getOpacity();
}

void GPUComponent::setTransform(const GPUWidgets::AffineTransform& newTransform)
{
    layer.setTransform(newTransform);
}

float GPUComponent::backingScale() const
{
    auto* host = findHost();
    return host != nullptr ? host->backingScale() : 1.f;
}

GPU::PixelFormat GPUComponent::colorFormat() const
{
    return layer.getColorFormat();
}

bool GPUComponent::wasCulledLastFrame() const
{
    return visibility == Visibility::Culled && findHost() != nullptr;
}

bool GPUComponent::wasHiddenLastFrame() const
{
    return visibility == Visibility::Hidden || findHost() == nullptr;
}

bool GPUComponent::isOnScreen() const
{
    return visibility != Visibility::Culled && isShowing();
}

Rect GPUComponent::getVisibleBounds() const
{
    return isOnScreen() ? visibleBounds : Rect {};
}

int GPUComponent::getRenderCount() const
{
    return renders;
}

void GPUComponent::setGpuTiming(bool shouldTime)
{
    if (gpuTiming == shouldTime)
        return;

    gpuTiming = shouldTime;
    gpuMilliseconds = 0.0;
    timedFrame = 0;

    if (gpuTiming && gpuLabel.empty())
    {
        static auto serial = std::atomic<int> {0};
        gpuLabel = "GPUComponent#" + std::to_string(++serial);
    }
}

bool GPUComponent::isGpuTiming() const
{
    return gpuTiming;
}

double GPUComponent::lastGpuMilliseconds() const
{
    if (!gpuTiming)
        return 0.0;

    collectGpuTime();
    return gpuMilliseconds;
}

std::string_view GPUComponent::timingLabel() const
{
    if (!gpuTiming)
        return {};

    return gpuLabel;
}

// The device keeps only the latest frame to come back, so this is read on every
// walk as well as when asked: a frame that rendered the component sets the
// figure, and one that did not leaves it.
void GPUComponent::collectGpuTime() const
{
    if (!gpuTiming)
        return;

    const auto& timings = GPU::Device::shared().lastFrameTimings();

    if (timings.frameIndex == timedFrame)
        return;

    timedFrame = timings.frameIndex;

    auto total = 0.0;
    auto found = false;

    for (const auto& pass: timings.passes)
    {
        if (isOwnPass(pass.label))
        {
            total += pass.milliseconds;
            found = true;
        }
    }

    if (found)
        gpuMilliseconds = total;
}

bool GPUComponent::isOwnPass(std::string_view label) const
{
    if (!label.starts_with(gpuLabel))
        return false;

    return label.size() == gpuLabel.size() || label[gpuLabel.size()] == '/';
}

void GPUComponent::targetChanged(GPU::Frame&) {}

const Layer& GPUComponent::getLayer() const
{
    return layer;
}

void GPUComponent::renderNow()
{
    layer.contentChanged();

    auto* host = findHost();

    if (host == nullptr || !isOnScreen())
        return;

    auto draw = [host](GPU::Frame& frame) { host->render(frame); };

    if (!host->renderFrame(draw))
        invalidateHost();
}

void GPUComponent::paint(Graphics& g)
{
    g.drawLayer(layer);
}

bool GPUComponent::advanceAnimation(double seconds)
{
    if (!continuous)
        return false;

    clock += seconds;
    sinceLastUpdate += seconds;

    if (!pacer.advance(seconds))
        return true;

    update({clock, sinceLastUpdate});
    sinceLastUpdate = 0.0;

    // Stale either way, so it renders the frame it comes back on screen; but
    // a frame is only asked for when there is something of it to see.
    layer.contentChanged();

    if (isOnScreen())
        invalidateHost();

    return true;
}

void GPUComponent::renderFrame(GPU::Frame& frame, const GPU::Texture& target)
{
    auto scale = frame.backingScale();

    if (renderedScale > 0.f && scale != renderedScale)
        onBackingScaleChanged(scale);

    renderedScale = scale;

    auto shape = TargetShape {target.sampleCount(),
                              target.hasDepth(),
                              target.hasStencil(),
                              target.format()};

    if (renderedTarget != shape)
    {
        renderedTarget = shape;
        targetChanged(frame);
    }

    ++renders;

    render(frame);
}

// A continuous component that left the host's list - removed, reparented, or
// made continuous before it had a host - joins it again.
void GPUComponent::resumeIfContinuous()
{
    if (!continuous || isAnimating() || findHost() == nullptr)
        return;

    pacer.reset();
    sinceLastUpdate = 0.0;
    startAnimating();
}

void GPUComponent::setVisibility(const Rect& visibleLocalBounds)
{
    auto culled = visibleLocalBounds.isEmpty();

    visibility = culled ? Visibility::Culled : Visibility::OnScreen;
    visibleBounds = culled ? Rect {} : visibleLocalBounds;
}

void GPUComponent::markHidden()
{
    visibility = Visibility::Hidden;
    visibleBounds = {};
}
} // namespace eacp::UI

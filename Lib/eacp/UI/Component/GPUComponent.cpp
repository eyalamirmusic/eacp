#include "GPUComponent.h"

#include "../Host/ComponentHost.h"

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
    return culled;
}

bool GPUComponent::isOnScreen() const
{
    return !culled && isShowing();
}

Rect GPUComponent::getVisibleBounds() const
{
    return isOnScreen() ? visibleBounds : Rect {};
}

int GPUComponent::getRenderCount() const
{
    return renders;
}

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

void GPUComponent::renderFrame(GPU::Frame& frame)
{
    auto scale = frame.backingScale();

    if (renderedScale > 0.f && scale != renderedScale)
        onBackingScaleChanged(scale);

    renderedScale = scale;
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
    culled = visibleLocalBounds.isEmpty();
    visibleBounds = culled ? Rect {} : visibleLocalBounds;
}
} // namespace eacp::UI

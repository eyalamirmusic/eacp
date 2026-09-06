#include "GPUView.h"

#include "../Device/Device.h"
#include "../Frame/Frame.h"

#include <eacp/Graphics/View/View-Linux.h>

// Linux/Vulkan placeholder. Two stages replace this file. Stage 3 gives
// renderNativeContent an off-screen target to render into and read back, which
// is the path 29 GPU test files ride on and the one that needs no compositor.
// Stage 4 gives the view a surface: VK_KHR_wayland_surface, a swapchain
// recreated on resize and on OUT_OF_DATE / SUBOPTIMAL, per-image semaphores,
// the framesInFlight clamp and DEVICE_LOST recovery through onDeviceRestored.
//
// Until then the settings are the whole of it. They are remembered rather than
// dropped - a caller sets sampleCount and depth before the view is on screen,
// and reading back something other than what was set would be wrong in a way
// that outlives the missing swapchain - and nothing is rendered, so render() is
// never called and renderNativeContent hands back an empty Image, which is the
// same answer View::renderNativeContent gives for a view with no native content
// (View-Linux.cpp).

namespace eacp::GPU
{
struct GPUView::Native
{
    explicit Native(GPUView&) {}

    int sampleCount = 1;
    int maxFps = 0;
    int framesInFlight = 2;
    bool depthEnabled = false;
    bool stencilEnabled = false;
    bool continuous = false;
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
}

void GPUView::setDepth(bool enabled)
{
    impl->depthEnabled = enabled;

    // The stencil plane lives in the depth attachment, so turning the
    // attachment off takes the plane with it rather than leaving hasStencil()
    // claiming a buffer that is gone.
    if (!enabled)
        impl->stencilEnabled = false;
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
}

bool GPUView::hasStencil() const
{
    return impl->stencilEnabled;
}

// Remembered and acted on by nothing: continuous rendering is a DisplayLink
// driving render(), and there is no frame to render yet.
void GPUView::setContinuous(bool continuous)
{
    impl->continuous = continuous;
}

bool GPUView::isContinuous() const
{
    return impl->continuous;
}

void GPUView::setMaxFps(int fps)
{
    impl->maxFps = fps;
}

int GPUView::maxFps() const
{
    return impl->maxFps;
}

void GPUView::setFramesInFlight(int count)
{
    impl->framesInFlight = count < 1 ? 1 : count;
}

int GPUView::framesInFlight() const
{
    return impl->framesInFlight;
}

void GPUView::resized()
{
    Graphics::View::resized();
}

void GPUView::backingScaleChanged()
{
    Graphics::View::backingScaleChanged();

    // Forwarded even with nothing to resize: Graphics::notifyBackingScaleChanged
    // is the seam stage 4's Wayland scale listener plugs into, and a glyph atlas
    // rebuilt from this callback is the reason it exists.
    onBackingScaleChanged(backingScale());
}

float GPUView::backingScale() const
{
    if (renderScale > 0.f)
        return renderScale;

    return Graphics::linuxDefaultBackingScale;
}

void GPUView::paint(Graphics::Context&)
{
    // Nothing to present and nothing to compose into. On the other two backends
    // this drives a live frame; here it would drive one that is invalid.
}

void GPUView::renderNow() {}

Graphics::Image GPUView::renderNativeContent(float)
{
    // Empty rather than a blank image of the right size: there is no off-screen
    // target to render into, so there are no pixels, and a caller that gets an
    // invalid Image knows that rather than comparing against a picture of
    // nothing. Stage 3 fills this in.
    return {};
}

bool GPUView::renderNativeContentToTarget(void*, float)
{
    return false;
}
} // namespace eacp::GPU

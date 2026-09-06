#include "GPUView.h"

#include "../Device/Device.h"
#include "../Frame/Frame.h"
#include "../Texture/Texture.h"

#include <eacp/Graphics/View/View-Linux.h>

#include <algorithm>
#include <cmath>
#include <cstdint>

// Linux/Vulkan backend, off-screen half. renderNativeContent renders the view
// into a texture it owns and reads the pixels back, which is the path
// View::renderToImage takes and the one the GPU test suite rides on - and it
// needs no compositor, no surface and no swapchain, which is why it lands a
// stage before them.
//
// Stage 4 gives the view a surface: VK_KHR_wayland_surface, a swapchain
// recreated on resize and on OUT_OF_DATE / SUBOPTIMAL, per-image semaphores,
// the framesInFlight clamp and DEVICE_LOST recovery through onDeviceRestored.
// Until then paint() and renderNow() have nothing to present to, and the
// settings below are remembered for the swapchain that will read them.

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

// Off-screen GPU snapshot for View::renderToImage, mirroring GPUView-Apple.mm
// and GPUView-Windows.cpp: render() draws into a texture this owns through a
// Frame that waits instead of presenting, then the texture is read back and
// swizzled from premultiplied BGRA to the straight RGBA an Image holds.
//
// The target is a plain GPU::Texture rather than a hand-built image, which is
// what this backend has that the other two do not: a render target created with
// renderTarget grows its own multisample companion and its own depth buffer, so
// the multisampling and the depth the view asked for come along by naming them
// in the descriptor, and OffscreenTarget's msaaTexture and depthTexture stay
// null.
//
// Empty on every failure - no device, an empty view, a texture the device would
// not make. An invalid Image says "there are no pixels", where a blank one of
// the right size would be a picture of nothing that a caller could compare
// against and believe.
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

    // BGRA rather than RGBA, so the bytes that come back are laid out the way
    // the swapchain will lay them out in stage 4 and the conversion below is
    // the same one on both paths.
    descriptor.format = TextureFormat::BGRA8Unorm;
    descriptor.renderTarget = true;

    // A count the device refuses would make the whole texture invalid and the
    // snapshot empty, where a view asking for more samples than the device has
    // should still get a picture. Texture refuses it for a target the caller
    // built; here the caller is this function.
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

        auto frame = Frame(device, target);

        // Rendered as a view of this scale: backingScale() answers it for the
        // duration, so a view sizing its geometry from the scale draws the
        // snapshot at the size that was asked for rather than at the screen's.
        renderScale = scale;
        render(frame);
        renderScale = 0.f;
    }
    // The Frame destructor submitted everything and waited, so the texture is
    // ready to be read.

    const auto pixelCount =
        static_cast<std::size_t>(pixelWidth) * static_cast<std::size_t>(pixelHeight);

    auto bgra = Vector<std::uint8_t>(static_cast<int>(pixelCount * 4));

    texture.read(bgra.data());

    auto image = Graphics::Image {};
    auto* dst = image.prepareForOverwrite(pixelWidth, pixelHeight);

    if (dst == nullptr)
        return {};

    // BGRA8 premultiplied - what a render target holds, and what the compositor
    // will treat the swapchain as - into the straight RGBA an Image holds.
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

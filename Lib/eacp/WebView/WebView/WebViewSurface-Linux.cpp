#include "WebViewSurface-Linux.h"

#include <eacp/Core/Threads/EventLoop.h>
#include <eacp/GPU/GPU.h>
#include <eacp/Graphics/View/View-Linux.h>

#include <cstddef>
#include <memory>
#include <optional>
#include <vector>

namespace eacp::Graphics::detail
{
namespace
{
struct WebViewSurfaceVertex
{
    float corner[2];
};

constexpr WebViewSurfaceVertex webViewSurfaceQuad[] = {
    {{0.0f, 0.0f}},
    {{1.0f, 0.0f}},
    {{1.0f, 1.0f}},
    {{0.0f, 0.0f}},
    {{1.0f, 1.0f}},
    {{0.0f, 1.0f}},
};

// The frame matches the drawable's pixels except between a resize and
// WebKit's answer to it, when it is stretched; smoothly, rather than aliased.
constexpr auto webViewSurfaceSampling = GPU::TextureSampling {
    GPU::TextureFilter::Linear, GPU::TextureAddressMode::Clamp};

// The frame is premultiplied, as the swapchain's composite alpha expects, and
// replaces what the cleared drawable holds rather than blending over it.
struct WebViewSurfaceShader final : GPU::ShaderProgram
{
    WebViewSurfaceShader()
    {
        image.sampling = webViewSurfaceSampling;
        compile();
    }

    void define() override
    {
        auto corner = vertexInput(&WebViewSurfaceVertex::corner);
        auto x = corner.x() * 2.0f - 1.0f;
        auto y = 1.0f - corner.y() * 2.0f;

        setPosition(float4(x, y, 0.0f, 1.0f));
        setFragment(sample(image, varying(corner)));
    }

    GPU::Uniform<GPU::Texture2D> image;

    EACP_SHADER(image)
};

// Ticks with nothing new before a static page's loop stops: one to be sure
// the frame that ended the animation was drawn, one more for a frame WebKit
// was about to hand over.
constexpr auto idleTicksBeforeStopping = 2;
} // namespace

struct WebViewSurface::Native
{
    Native(WebViewSurface& ownerToUse, const WpeFrame& frameToShow)
        : owner(ownerToUse)
        , frame(frameToShow)
        , surface(requestViewSurface(ownerToUse))
    {
    }

    bool uploadFreshFrame()
    {
        if (!fresh || frame.isEmpty())
            return false;

        fresh = false;

        if (texture.has_value() && texture->width() == frame.width()
            && texture->height() == frame.height())
        {
            texture->update(frame.data());
            return true;
        }

        auto descriptor = GPU::TextureDescriptor {};
        descriptor.width = frame.width();
        descriptor.height = frame.height();
        descriptor.format = GPU::TextureFormat::BGRA8Unorm;

        texture = GPU::Device::shared().makeTexture(descriptor, frame.data());

        if (!texture->isValid())
            texture.reset();

        return true;
    }

    WebViewSurfaceShader* shaderFor(GPU::TextureFormat format)
    {
        if (!shader.has_value() || shaderFormat != format)
        {
            shader.emplace();
            shader->setVertices(webViewSurfaceQuad);
            shader->prepare(1,
                            false,
                            GPU::PrimitiveTopology::Triangles,
                            GPU::BlendMode::None,
                            GPU::pixelFormatFor(format));
            shaderFormat = format;
        }

        return &*shader;
    }

    void draw(GPU::Frame& target)
    {
        auto descriptor = GPU::RenderPassDescriptor {};
        descriptor.clearColor = Color::transparent();

        auto pass = target.beginPass(descriptor);

        if (!texture.has_value())
            return;

        auto* program = shaderFor(target.drawableFormat());
        program->image = *texture;
        pass.draw(*program);
    }

    bool recordReadBack(GPU::Frame& target)
    {
        if (!readBackRequested || !owner.hasReadableDrawable())
            return false;

        auto size = target.pixelSize();

        auto descriptor = GPU::TextureDescriptor {};
        descriptor.width = (int) size.x;
        descriptor.height = (int) size.y;
        descriptor.format = target.drawableFormat();

        auto copy = GPU::Device::shared().makeTexture(descriptor);

        if (!copy.isValid() || !target.copyDrawable(copy))
            return false;

        readBackRequested = false;
        readBackTexture = std::move(copy);
        return true;
    }

    void finishReadBack()
    {
        if (!readBackTexture.has_value())
            return;

        auto& copy = *readBackTexture;
        auto width = copy.width();
        auto height = copy.height();
        auto pixels = std::vector<std::uint8_t>((std::size_t) width * height * 4);

        copy.read(pixels.data());

        auto result = PresentedPixels {};
        result.width = width;
        result.height = height;

        auto centre = ((std::size_t) (height / 2) * width + width / 2) * 4;

        for (auto channel = 0; channel < 4; ++channel)
            result.centre[(std::size_t) channel] = pixels[centre + channel];

        readBack = result;
        readBackTexture.reset();
    }

    // The frame's present is queued once render() has returned and the Frame
    // has ended, so what it owes is said on the next turn.
    void reportAfterPresent(bool presentedNewFrame)
    {
        Threads::callAsync(
            [this, weak = std::weak_ptr<bool> {alive}, presentedNewFrame]
            {
                if (weak.expired())
                    return;

                finishReadBack();

                if (presentedNewFrame)
                    owner.onPresented();
            });
    }

    // Not from inside the tick: the continuous loop's callback is what is
    // running, and stopping the loop replaces it.
    void stopWhenStillIdle()
    {
        Threads::callAsync(
            [this, weak = std::weak_ptr<bool> {alive}]
            {
                if (weak.expired() || fresh || idleTicks < idleTicksBeforeStopping)
                    return;

                owner.setContinuous(false);
            });
    }

    WebViewSurface& owner;
    const WpeFrame& frame;
    ViewSurface& surface;

    std::optional<GPU::Texture> texture;
    std::optional<WebViewSurfaceShader> shader;
    GPU::TextureFormat shaderFormat = GPU::TextureFormat::BGRA8Unorm;

    bool fresh = false;
    int idleTicks = 0;
    int presented = 0;

    bool readBackRequested = false;
    std::optional<GPU::Texture> readBackTexture;
    std::optional<PresentedPixels> readBack;

    // A weak copy fences the callbacks queued for a later turn.
    std::shared_ptr<bool> alive = std::make_shared<bool>(true);
};

WebViewSurface::WebViewSurface(const WpeFrame& frameToShow)
    : impl(*this, frameToShow)
{
    setSampleCount(1);
    setIgnoresMouseEvents(true);
}

WebViewSurface::~WebViewSurface() = default;

bool WebViewSurface::canPresent() const
{
    const auto& surface = impl->surface;

    return surface.handle.isValid() && surface.pixelWidth > 0
           && surface.pixelHeight > 0;
}

void WebViewSurface::frameArrived()
{
    impl->fresh = true;
    impl->idleTicks = 0;

    if (!isContinuous())
        setContinuous(true);
}

int WebViewSurface::presentedFrames() const
{
    return impl->presented;
}

void WebViewSurface::readBackNextFrame()
{
    impl->readBack.reset();
    impl->readBackRequested = true;
    setReadableDrawable(true);
    repaint();
}

bool WebViewSurface::isReadBackPending() const
{
    return impl->readBackRequested || impl->readBackTexture.has_value();
}

std::optional<PresentedPixels> WebViewSurface::lastPresentedPixels() const
{
    return impl->readBack;
}

void WebViewSurface::render(GPU::Frame& frame)
{
    auto presentedNewFrame = impl->uploadFreshFrame();

    impl->draw(frame);

    auto readingBack = impl->recordReadBack(frame);

    if (presentedNewFrame)
        ++impl->presented;

    if (presentedNewFrame || readingBack)
        impl->reportAfterPresent(presentedNewFrame);
}

void WebViewSurface::update(Threads::FrameTime)
{
    if (impl->fresh)
    {
        impl->idleTicks = 0;
        return;
    }

    if (++impl->idleTicks >= idleTicksBeforeStopping)
        impl->stopWhenStillIdle();
}
} // namespace eacp::Graphics::detail

#include "VideoView.h"

#include <eacp/GPU/GPU.h>
#include <eacp/Graphics/Graphics.h>

#include <algorithm>

namespace eacp::Video
{
namespace
{
// A video frame is fitted to its rect by an arbitrary factor, so it has to be
// filtered smoothly; SpriteRenderer's default Nearest would alias it badly.
constexpr auto frameSampling = GPU::TextureSampling {GPU::TextureFilter::Linear,
                                                     GPU::TextureAddressMode::Clamp};
} // namespace

Graphics::Rect
    fitImageArea(const Graphics::Rect& dst, int imageWidth, int imageHeight, Fit fit)
{
    if (fit == Fit::Stretch || imageWidth <= 0 || imageHeight <= 0 || dst.w <= 0.0f
        || dst.h <= 0.0f)
        return dst;

    auto imageAspect = (float) imageWidth / (float) imageHeight;
    auto dstAspect = dst.w / dst.h;
    auto imageWider = imageAspect > dstAspect;

    // Contain fits inside, so the wider dimension becomes the limit; Cover fills,
    // so the narrower one does and the other overflows.
    auto widthLimited = fit == Fit::Contain ? imageWider : !imageWider;

    auto width = widthLimited ? dst.w : dst.h * imageAspect;
    auto height = widthLimited ? dst.w / imageAspect : dst.h;

    return {dst.x + (dst.w - width) * 0.5f,
            dst.y + (dst.h - height) * 0.5f,
            width,
            height};
}

Graphics::Rect FramePresenter::drawFitted(Sprites::SpriteRenderer& renderer,
                                          const GPU::Texture& texture,
                                          const Graphics::Rect& dst,
                                          Fit fit,
                                          const Graphics::Color& tint)
{
    if (!texture.isValid() || texture.width() <= 0 || texture.height() <= 0
        || dst.isEmpty())
        return {};

    auto textureWidth = (float) texture.width();
    auto textureHeight = (float) texture.height();

    if (fit == Fit::Cover)
    {
        // Cover crops through the source rect rather than overflowing dst: a
        // presenter shares its view with the others, so overflow would draw
        // over its neighbours.
        auto scale = std::max(dst.w / textureWidth, dst.h / textureHeight);
        auto sourceWidth = dst.w / scale;
        auto sourceHeight = dst.h / scale;

        auto source = Graphics::Rect {(textureWidth - sourceWidth) * 0.5f,
                                      (textureHeight - sourceHeight) * 0.5f,
                                      sourceWidth,
                                      sourceHeight};

        renderer.drawTexture(texture, source, dst, tint, frameSampling);
        return dst;
    }

    auto imageArea = fitImageArea(dst, texture.width(), texture.height(), fit);
    renderer.drawTexture(texture, imageArea, false, false, tint, frameSampling);
    return imageArea;
}

void FramePresenter::setUploadMode(UploadMode mode)
{
    uploadMode = mode;
}

Graphics::Rect FramePresenter::drawZeroCopy(Player& player,
                                            Sprites::SpriteRenderer& renderer,
                                            const Graphics::Rect& dst,
                                            Fit fit,
                                            const Graphics::Color& tint)
{
    // Sequence first, buffer second: a frame published between the two reads
    // then costs one redundant re-wrap on the next render. The other order
    // stamps the older buffer with the newer number, and serves the STALE
    // wrap when that frame comes due.
    auto sequence = player.frameSequence();
    auto* buffer = player.acquireLatestPixelBuffer();

    if (buffer == nullptr)
        return {};

    if (!wrappedTexture.has_value() || sequence != wrappedSequence)
    {
        wrappedTexture.emplace(GPU::Device::shared().wrapPixelBuffer(buffer));
        wrappedSequence = sequence;
    }

    // The wrapped texture holds its own reference to the frame's surface, so the
    // buffer can be released now.
    Player::releasePixelBuffer(buffer);

    return drawFitted(renderer, *wrappedTexture, dst, fit, tint);
}

Graphics::Rect FramePresenter::drawCpuUpload(Player& player,
                                             Sprites::SpriteRenderer& renderer,
                                             const Graphics::Rect& dst,
                                             Fit fit,
                                             const Graphics::Color& tint)
{
    if (player.copyLatestFrame(scratch))
    {
        auto sizeChanged = !uploadTexture.has_value()
                           || uploadTexture->width() != scratch.width
                           || uploadTexture->height() != scratch.height;

        if (sizeChanged)
        {
            auto descriptor = GPU::TextureDescriptor {};
            descriptor.width = scratch.width;
            descriptor.height = scratch.height;
            descriptor.format = GPU::TextureFormat::BGRA8Unorm;
            uploadTexture.emplace(GPU::Device::shared().makeTexture(descriptor));
        }

        uploadTexture->update(scratch.data.data());
    }

    if (!uploadTexture.has_value() || scratch.width <= 0)
        return {};

    return drawFitted(renderer, *uploadTexture, dst, fit, tint);
}

// Frame gating is per player — sequence numbers from different players are
// unrelated — so a presenter handed a new player drops everything it cached
// from the old one rather than serving a stale frame.
void FramePresenter::resetForPlayer(const Player& player)
{
    if (boundPlayer == &player)
        return;

    boundPlayer = &player;
    wrappedTexture.reset();
    wrappedSequence = 0;
    uploadTexture.reset();
    scratch = {};
}

Graphics::Rect FramePresenter::draw(Player& player,
                                    Sprites::SpriteRenderer& renderer,
                                    const Graphics::Rect& dst,
                                    Fit fit,
                                    const Graphics::Color& tint)
{
    resetForPlayer(player);

    auto imageArea = Graphics::Rect {};

    if (uploadMode != UploadMode::Copy)
        imageArea = drawZeroCopy(player, renderer, dst, fit, tint);

    if (imageArea.isEmpty() && uploadMode != UploadMode::ZeroCopy)
        imageArea = drawCpuUpload(player, renderer, dst, fit, tint);

    return imageArea;
}

VideoView::VideoView()
{
    // Video is already smooth continuous-tone content, so MSAA buys nothing;
    // keep it at one sample.
    setSampleCount(1);

    arrivalTick = Threads::DisplayLink::timedTick(
        [this](Threads::FrameTime time)
        {
            update(time);
            renderNow();
        });
}

VideoView::~VideoView()
{
    *alive = false;

    if (player != nullptr)
        player->setFrameArrivedCallback({});
}

void VideoView::attach(Player& playerToUse)
{
    player = &playerToUse;
    applyRenderMode();
    repaint();
}

void VideoView::detach()
{
    if (player != nullptr)
        player->setFrameArrivedCallback({});

    player = nullptr;
    setContinuous(false);
    repaint();
}

void VideoView::setFit(Fit fitToUse)
{
    fit = fitToUse;
}

void VideoView::setUploadMode(UploadMode mode)
{
    presenter.setUploadMode(mode);
}

void VideoView::setRenderMode(RenderMode mode)
{
    renderMode = mode;
    applyRenderMode();
}

void VideoView::applyRenderMode()
{
    setContinuous(renderMode == RenderMode::Continuous);

    if (player == nullptr)
        return;

    if (renderMode != RenderMode::OnFrameArrival)
    {
        player->setFrameArrivedCallback({});
        return;
    }

    // Fires on the player's decode thread; the render is marshalled to the
    // main thread, where the alive token fences it against a torn-down view.
    player->setFrameArrivedCallback(
        [this, guard = alive]
        {
            Threads::callAsync(
                [this, guard]
                {
                    if (*guard)
                        arrivalTick();
                });
        });
}

void VideoView::ensureRenderer()
{
    auto bounds = getLocalBounds();
    auto size = Graphics::Point {bounds.w, bounds.h};

    // Constructed once; a resize only retargets the logical space. Rebuilding
    // the renderer would recompile its pipelines on every tick of a live
    // resize.
    if (!renderer.has_value())
        renderer.emplace(size, sampleCount());
    else
        renderer->setLogicalSize(size);
}

void VideoView::render(GPU::Frame& frame)
{
    ensureRenderer();

    auto pass = frame.beginPass({Graphics::Color::black()});
    renderer->begin(pass);

    auto imageArea = Graphics::Rect {};

    if (player != nullptr)
        imageArea = presenter.draw(*player, *renderer, getLocalBounds(), fit);

    drawOverlay(*renderer, imageArea);
}

void VideoView::drawOverlay(Sprites::SpriteRenderer&, const Graphics::Rect&) {}
} // namespace eacp::Video

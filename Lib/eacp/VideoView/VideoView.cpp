#include "VideoView.h"

#include <eacp/GPU/GPU.h>

namespace eacp::Video
{
namespace
{
// A video frame is fitted to the view by an arbitrary factor, so it has to be
// filtered smoothly; SpriteRenderer's default Nearest would alias it badly.
constexpr auto frameSampling = GPU::TextureSampling {GPU::TextureFilter::Linear,
                                                     GPU::TextureAddressMode::Clamp};
} // namespace

VideoView::VideoView()
{
    // Decoded video is already smooth, and the image is a single quad, so MSAA
    // buys nothing; keep it at one sample.
    setSampleCount(1);
}

VideoView::~VideoView() = default;

void VideoView::attach(Player& playerToUse)
{
    player = &playerToUse;
    stream = &playerToUse.stream();

    setContinuous(true);
    repaint();
}

void VideoView::attach(FrameStream& streamToUse)
{
    player = nullptr;
    stream = &streamToUse;

    setContinuous(false);
    repaint();
}

void VideoView::detach()
{
    player = nullptr;
    stream = nullptr;

    setContinuous(false);
    repaint();
}

void VideoView::setTime(double seconds)
{
    playhead = seconds;
    repaint();
}

double VideoView::time() const
{
    return player != nullptr ? player->position() : playhead;
}

void VideoView::setFit(Fit fitToUse)
{
    fit = fitToUse;
}

void VideoView::setMirrored(bool mirroredToUse)
{
    mirrored = mirroredToUse;
}

void VideoView::setUploadMode(UploadMode mode)
{
    uploadMode = mode;
}

void VideoView::update(Threads::FrameTime frameTime)
{
    if (player != nullptr)
        player->advance(frameTime.delta);
}

void VideoView::ensureRenderer()
{
    auto bounds = getLocalBounds();
    auto size = Graphics::Point {bounds.w, bounds.h};

    if (!renderer.has_value() || size.x != rendererSize.x
        || size.y != rendererSize.y)
    {
        renderer.emplace(size, sampleCount());
        rendererSize = size;
    }
}

Graphics::Rect VideoView::imageAreaFor(int textureWidth, int textureHeight) const
{
    auto bounds = getLocalBounds();
    return Sprites::fitRect(bounds.w, bounds.h, textureWidth, textureHeight, fit);
}

bool VideoView::drawZeroCopy(const VideoFrame& frame, Graphics::Rect& imageArea)
{
    auto* buffer = frame.nativeBuffer();

    if (buffer == nullptr)
        return false;

    // The frame keeps the buffer alive for as long as this call runs, so unlike
    // the camera path there is nothing to acquire and release around the wrap.
    auto texture = GPU::Device::shared().wrapPixelBuffer(buffer);

    if (!texture.isValid())
        return false;

    imageArea = imageAreaFor(texture.width(), texture.height());
    renderer->drawTexture(texture,
                          imageArea,
                          mirrored,
                          false,
                          Graphics::Color::white(),
                          frameSampling);
    return true;
}

bool VideoView::drawUpload(const VideoFrame& frame, Graphics::Rect& imageArea)
{
    const auto* pixels = frame.pixels();

    if (pixels == nullptr || frame.width() <= 0 || frame.height() <= 0)
        return false;

    auto sizeChanged = !uploadTexture.has_value()
                       || uploadTexture->width() != frame.width()
                       || uploadTexture->height() != frame.height();

    if (sizeChanged)
    {
        auto descriptor = GPU::TextureDescriptor {};
        descriptor.width = frame.width();
        descriptor.height = frame.height();
        descriptor.format = GPU::TextureFormat::BGRA8Unorm;
        uploadTexture.emplace(GPU::Device::shared().makeTexture(descriptor));
        uploadedPixels = nullptr;
    }

    if (!uploadTexture->isValid())
        return false;

    // Redrawing the same frame — a resize, an overlay change — should not pay
    // for the upload again.
    if (uploadedPixels != pixels)
    {
        uploadTexture->update(pixels, frame.bytesPerRow());
        uploadedPixels = pixels;
    }

    imageArea = imageAreaFor(frame.width(), frame.height());
    renderer->drawTexture(*uploadTexture,
                          imageArea,
                          mirrored,
                          false,
                          Graphics::Color::white(),
                          frameSampling);
    return true;
}

void VideoView::render(GPU::Frame& frame)
{
    ensureRenderer();

    auto pass = frame.beginPass({Graphics::Color::black()});
    renderer->begin(pass);

    auto imageArea = Graphics::Rect {};

    if (stream != nullptr)
    {
        // Held for the whole pass: it owns the pixels the GPU is about to read,
        // and the decode thread is free to keep filling the queue meanwhile.
        auto videoFrame =
            player != nullptr ? player->currentFrame() : stream->frameAt(playhead);

        if (videoFrame.isValid())
        {
            auto drew = false;

            if (uploadMode != UploadMode::Copy)
                drew = drawZeroCopy(videoFrame, imageArea);

            if (!drew && uploadMode != UploadMode::ZeroCopy)
                drawUpload(videoFrame, imageArea);
        }
    }

    drawOverlay(*renderer, imageArea);
}

void VideoView::drawOverlay(Sprites::SpriteRenderer&, const Graphics::Rect&) {}
} // namespace eacp::Video

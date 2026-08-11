#include "VideoView.h"

#include <eacp/GPU/GPU.h>

namespace eacp::Video
{
namespace
{
// Frames are fitted by an arbitrary factor, so the default Nearest aliases.
constexpr auto frameSampling = GPU::TextureSampling {GPU::TextureFilter::Linear,
                                                     GPU::TextureAddressMode::Clamp};
} // namespace

VideoView::VideoView()
{
    setSampleCount(1);
}

VideoView::~VideoView()
{
    // Must not touch `stream`: a subclass holding it as a member has already
    // destroyed it by now. Clearing the token is enough, since the stored
    // callback only reads `alive` before hopping to the main thread.
    *alive = false;
}

void VideoView::attach(Player& playerToUse)
{
    detach();

    player = &playerToUse;
    stream = &playerToUse.stream();

    // The player advances every refresh, so the arrival hook would only add
    // redundant repaints.
    setContinuous(true);
    repaint();
}

void VideoView::attach(FrameStream& streamToUse)
{
    detach();

    stream = &streamToUse;

    setContinuous(false);
    applyFrameReadyCallback();
    repaint();
}

void VideoView::detach()
{
    if (stream != nullptr)
        stream->setFrameReadyCallback({});

    player = nullptr;
    stream = nullptr;

    setContinuous(false);
    repaint();
}

void VideoView::applyFrameReadyCallback()
{
    if (stream == nullptr)
        return;

    // On-demand rendering, so a frame landing after setTime() was served — the
    // common case after a seek — needs this to trigger the redraw.
    stream->setFrameReadyCallback(
        [this, guard = alive]
        {
            Threads::callAsync(
                [this, guard]
                {
                    if (*guard)
                        repaint();
                });
        });
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
    auto size = Point {bounds.w, bounds.h};

    if (!renderer.has_value() || size.x != rendererSize.x
        || size.y != rendererSize.y)
    {
        renderer.emplace(size, sampleCount());
        rendererSize = size;
    }
}

Point VideoView::displaySize(int textureWidth,
                             int textureHeight,
                             int rotationDegrees)
{
    auto quarterTurn = rotationDegrees == 90 || rotationDegrees == 270;

    return quarterTurn ? Point {(float) textureHeight, (float) textureWidth}
                       : Point {(float) textureWidth, (float) textureHeight};
}

VideoView::Placement
    VideoView::computePlacement(const Rect& area, int rotationDegrees, bool mirrored)
{
    auto left = area.x;
    auto top = area.y;
    auto right = area.x + area.w;
    auto bottom = area.y + area.h;

    auto placement = Placement {};

    // +u runs rightwards in the texture, +v downwards.
    switch (((rotationDegrees % 360) + 360) % 360)
    {
        case 90:
            placement = {{right, top}, {0.0f, area.h}, {-area.w, 0.0f}};
            break;
        case 180:
            placement = {{right, bottom}, {-area.w, 0.0f}, {0.0f, -area.h}};
            break;
        case 270:
            placement = {{left, bottom}, {0.0f, -area.h}, {area.w, 0.0f}};
            break;
        default:
            placement = {{left, top}, {area.w, 0.0f}, {0.0f, area.h}};
            break;
    }

    // Applied after the rotation, so it flips the *displayed* image.
    if (mirrored)
    {
        placement.origin = {placement.origin.x + placement.edgeX.x,
                            placement.origin.y + placement.edgeX.y};
        placement.edgeX = {-placement.edgeX.x, -placement.edgeX.y};
    }

    return placement;
}

Rect VideoView::imageAreaFor(int textureWidth, int textureHeight) const
{
    auto bounds = getLocalBounds();
    auto shown = displaySize(textureWidth, textureHeight, rotation);

    return Sprites::fitRect(bounds.w, bounds.h, (int) shown.x, (int) shown.y, fit);
}

void VideoView::drawFrameTexture(const GPU::Texture& texture, Rect& imageArea)
{
    imageArea = imageAreaFor(texture.width(), texture.height());

    auto placement = computePlacement(imageArea, rotation, mirrored);

    renderer->drawTextureQuad(texture,
                              placement.origin,
                              placement.edgeX,
                              placement.edgeY,
                              Color::white(),
                              frameSampling);
}

bool VideoView::drawZeroCopy(const VideoFrame& frame, Rect& imageArea)
{
    auto* buffer = frame.nativeBuffer();

    if (buffer == nullptr)
        return false;

    // The frame keeps the buffer alive across this call, so nothing has to be
    // acquired and released around the wrap.
    auto texture = GPU::Device::shared().wrapPixelBuffer(buffer);

    if (!texture.isValid())
        return false;

    drawFrameTexture(texture, imageArea);
    return true;
}

bool VideoView::drawUpload(const VideoFrame& frame, Rect& imageArea)
{
    const auto* pixels = frame.pixels();

    if (pixels == nullptr || frame.width() <= 0 || frame.height() <= 0)
        return false;

    auto isNv12 = frame.format() == FramePixelFormat::NV12;

    auto rebuild = !uploadTexture.has_value()
                   || uploadTexture->width() != frame.width()
                   || uploadTexture->height() != frame.height()
                   || uploadedFormat != frame.format();

    if (rebuild)
    {
        auto descriptor = GPU::TextureDescriptor {};
        descriptor.width = frame.width();
        descriptor.height = frame.height();
        descriptor.format =
            isNv12 ? GPU::TextureFormat::R8Unorm : GPU::TextureFormat::BGRA8Unorm;
        uploadTexture.emplace(GPU::Device::shared().makeTexture(descriptor));

        if (isNv12)
        {
            // One Cb/Cr pair per 2x2 block of luma.
            descriptor.width = frame.width() / 2;
            descriptor.height = frame.height() / 2;
            descriptor.format = GPU::TextureFormat::RG8Unorm;
            chromaTexture.emplace(GPU::Device::shared().makeTexture(descriptor));
        }
        else
        {
            chromaTexture.reset();
        }

        uploadedFormat = frame.format();
        uploadedPixels = nullptr;
    }

    if (!uploadTexture->isValid()
        || (isNv12 && !(chromaTexture.has_value() && chromaTexture->isValid())))
        return false;

    if (uploadedPixels != pixels)
    {
        uploadTexture->update(pixels, frame.bytesPerRow());

        if (isNv12)
            chromaTexture->update(frame.chromaPlane(), frame.bytesPerRow());

        uploadedPixels = pixels;
    }

    if (!isNv12)
    {
        drawFrameTexture(*uploadTexture, imageArea);
        return true;
    }

    imageArea = imageAreaFor(uploadTexture->width(), uploadTexture->height());
    auto placement = computePlacement(imageArea, rotation, mirrored);

    auto yuv = frame.yuvTransform();

    renderer->drawNv12Quad(*uploadTexture,
                           *chromaTexture,
                           {yuv.lumaOffset,
                            yuv.lumaScale,
                            yuv.chromaOffset,
                            yuv.chromaScale,
                            yuv.redV,
                            yuv.greenU,
                            yuv.greenV,
                            yuv.blueU},
                           placement.origin,
                           placement.edgeX,
                           placement.edgeY,
                           Color::white(),
                           frameSampling);
    return true;
}

void VideoView::render(GPU::Frame& frame)
{
    ensureRenderer();

    auto pass = frame.beginPass({Color::black()});
    renderer->begin(pass);

    auto imageArea = Rect {};

    if (stream != nullptr)
    {
        // Read per frame, since an attached stream can be reopened on a
        // different file.
        rotation = stream->info().rotationDegrees;

        // Held for the whole pass: it owns the pixels the GPU is about to read.
        auto videoFrame =
            player != nullptr ? player->currentFrame() : stream->frameAt(playhead);

        if (videoFrame.isValid())
        {
            auto drew = false;

            if (uploadMode != UploadMode::Copy)
                drew = drawZeroCopy(videoFrame, imageArea);

            zeroCopyLastFrame = drew;

            if (!drew && uploadMode != UploadMode::ZeroCopy)
                drawUpload(videoFrame, imageArea);
        }
    }

    drawOverlay(pass, *renderer, imageArea);
}

void VideoView::drawOverlay(GPU::RenderPass&, Sprites::SpriteRenderer&, const Rect&)
{
}
} // namespace eacp::Video

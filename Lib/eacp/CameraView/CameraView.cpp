#include "CameraView.h"

#include <eacp/GPU/GPU.h>

namespace eacp::Cameras
{
CameraView::CameraView()
{
    // The camera feed is already smooth video, so MSAA buys nothing; keep it at
    // one sample. Continuous mode redraws every vsync to show new frames as they
    // arrive, decoupled from the capture rate.
    setSampleCount(1);
    setContinuous(true);
}

CameraView::~CameraView() = default;

void CameraView::attach(Camera& cameraToUse)
{
    camera = &cameraToUse;
    repaint();
}

void CameraView::detach()
{
    camera = nullptr;
    repaint();
}

void CameraView::setFit(Fit fitToUse)
{
    fit = fitToUse;
}

void CameraView::setMirrored(bool mirroredToUse)
{
    mirrored = mirroredToUse;
}

Graphics::Rect CameraView::computeImageArea(
    float viewWidth, float viewHeight, int textureWidth, int textureHeight, Fit fit)
{
    if (fit == Fit::Stretch || textureWidth <= 0 || textureHeight <= 0
        || viewWidth <= 0.0f || viewHeight <= 0.0f)
        return {0.0f, 0.0f, viewWidth, viewHeight};

    auto imageAspect = (float) textureWidth / (float) textureHeight;
    auto viewAspect = viewWidth / viewHeight;
    auto imageWider = imageAspect > viewAspect;

    // Contain fits inside, so the wider dimension becomes the limit; Cover fills,
    // so the narrower one does and the other overflows.
    auto widthLimited = fit == Fit::Contain ? imageWider : !imageWider;

    auto width = widthLimited ? viewWidth : viewHeight * imageAspect;
    auto height = widthLimited ? viewWidth / imageAspect : viewHeight;

    auto x = (viewWidth - width) * 0.5f;
    auto y = (viewHeight - height) * 0.5f;

    return {x, y, width, height};
}

void CameraView::ensureRenderer()
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

void CameraView::render(GPU::Frame& frame)
{
    ensureRenderer();

    auto pass = frame.beginPass({Graphics::Color::black()});
    renderer->begin(pass);

    auto imageArea = Graphics::Rect {};

    if (camera != nullptr)
    {
        auto* buffer = camera->acquireLatestPixelBuffer();

        if (buffer != nullptr)
        {
            auto texture = GPU::Device::shared().wrapPixelBuffer(buffer);

            if (texture.isValid())
            {
                auto bounds = getLocalBounds();
                imageArea = computeImageArea(
                    bounds.w, bounds.h, texture.width(), texture.height(), fit);
                renderer->drawTexture(texture, imageArea, mirrored, false);
            }

            // The wrapped texture holds its own reference to the frame's surface,
            // so the buffer can be released now.
            Camera::releasePixelBuffer(buffer);
        }
    }

    drawOverlay(*renderer, imageArea);
}

void CameraView::drawOverlay(Sprites::SpriteRenderer&, const Graphics::Rect&) {}
} // namespace eacp::Cameras

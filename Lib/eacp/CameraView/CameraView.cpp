#include "CameraView.h"

#include <eacp/GPU/GPU.h>
#include <eacp/Graphics/Graphics.h>

namespace eacp::Cameras
{
CameraView::CameraView()
{
    // The camera feed is already smooth video, so MSAA buys nothing; keep it at
    // one sample.
    setSampleCount(1);

    arrivalTick = Threads::DisplayLink::timedTick(
        [this](Threads::FrameTime time)
        {
            update(time);
            renderNow();
        });
}

CameraView::~CameraView()
{
    *alive = false;

    if (camera != nullptr)
        camera->setFrameArrivedCallback({});
}

void CameraView::attach(Camera& cameraToUse)
{
    camera = &cameraToUse;
    applyRenderMode();
    repaint();
}

void CameraView::detach()
{
    if (camera != nullptr)
        camera->setFrameArrivedCallback({});

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

void CameraView::setUploadMode(UploadMode mode)
{
    uploadMode = mode;
}

void CameraView::setRenderMode(RenderMode mode)
{
    renderMode = mode;
    applyRenderMode();
}

void CameraView::applyRenderMode()
{
    setContinuous(renderMode == RenderMode::Continuous);

    if (camera == nullptr)
        return;

    if (renderMode != RenderMode::OnFrameArrival)
    {
        camera->setFrameArrivedCallback({});
        return;
    }

    // Fires on the capture thread; the render is marshalled to the main
    // thread, where the alive token fences it against a torn-down view.
    camera->setFrameArrivedCallback(
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

Graphics::Rect CameraView::imageAreaFor(int textureWidth, int textureHeight) const
{
    auto bounds = getLocalBounds();
    return computeImageArea(bounds.w, bounds.h, textureWidth, textureHeight, fit);
}

bool CameraView::renderZeroCopy(Graphics::Rect& imageArea)
{
    auto* buffer = camera->acquireLatestPixelBuffer();

    if (buffer == nullptr)
        return false;

    auto texture = GPU::Device::shared().wrapPixelBuffer(buffer);
    auto drew = false;

    if (texture.isValid())
    {
        imageArea = imageAreaFor(texture.width(), texture.height());
        renderer->drawTexture(texture, imageArea, mirrored, false);
        drew = true;
    }

    // The wrapped texture holds its own reference to the frame's surface, so the
    // buffer can be released now.
    Camera::releasePixelBuffer(buffer);
    return drew;
}

bool CameraView::renderCpuUpload(Graphics::Rect& imageArea)
{
    if (camera->copyLatestFrame(scratch))
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

    if (!uploadTexture.has_value() || !uploadTexture->isValid()
        || scratch.width <= 0)
        return false;

    imageArea = imageAreaFor(scratch.width, scratch.height);
    renderer->drawTexture(*uploadTexture, imageArea, mirrored, false);
    return true;
}

void CameraView::render(GPU::Frame& frame)
{
    ensureRenderer();

    auto pass = frame.beginPass({Graphics::Color::black()});
    renderer->begin(pass);

    auto imageArea = Graphics::Rect {};

    if (camera != nullptr)
    {
        auto drew = false;

        if (uploadMode != UploadMode::Copy)
            drew = renderZeroCopy(imageArea);

        if (!drew && uploadMode != UploadMode::ZeroCopy)
            renderCpuUpload(imageArea);
    }

    drawOverlay(*renderer, imageArea);
}

void CameraView::drawOverlay(Sprites::SpriteRenderer&, const Graphics::Rect&) {}
} // namespace eacp::Cameras

#include "CameraView.h"

#include <eacp/GPU/GPU.h>
#include <eacp/Graphics/Graphics.h>

namespace eacp::Cameras
{
namespace
{
// A camera frame is fitted to the view by an arbitrary factor, so it has to be
// filtered smoothly; SpriteRenderer's default Nearest would alias it badly.
constexpr auto frameSampling = GPU::TextureSampling {GPU::TextureFilter::Linear,
                                                     GPU::TextureAddressMode::Clamp};

int normalisedRotation(int rotationDegrees)
{
    return ((rotationDegrees % 360) + 360) % 360;
}

bool isQuarterTurn(int rotationDegrees)
{
    auto rotation = normalisedRotation(rotationDegrees);
    return rotation == 90 || rotation == 270;
}

struct Placement
{
    Graphics::Point origin;
    Graphics::Point edgeX;
    Graphics::Point edgeY;
};

// Where the texture's top-left corner lands and where its +u and +v axes run,
// turned clockwise by rotationDegrees and then mirrored as displayed.
Placement
    placementFor(const Graphics::Rect& area, int rotationDegrees, bool mirrored)
{
    auto left = area.x;
    auto top = area.y;
    auto right = area.x + area.w;
    auto bottom = area.y + area.h;

    auto placement = Placement {};

    switch (normalisedRotation(rotationDegrees))
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

    if (mirrored)
    {
        auto turned = isQuarterTurn(rotationDegrees);
        auto& horizontal = turned ? placement.edgeY : placement.edgeX;

        placement.origin = {placement.origin.x + horizontal.x,
                            placement.origin.y + horizontal.y};
        horizontal = {-horizontal.x, -horizontal.y};
    }

    return placement;
}

constexpr Sprites::YuvTransform yuvTransformFor(YuvMatrix matrix, bool fullRange)
{
    auto kr = matrix == YuvMatrix::BT709 ? 0.2126f : 0.299f;
    auto kb = matrix == YuvMatrix::BT709 ? 0.0722f : 0.114f;
    auto kg = 1.0f - kr - kb;

    auto transform = Sprites::YuvTransform {};

    transform.lumaOffset = fullRange ? 0.0f : 16.0f / 255.0f;
    transform.lumaScale = fullRange ? 1.0f : 255.0f / 219.0f;
    transform.chromaOffset = 128.0f / 255.0f;
    transform.chromaScale = fullRange ? 1.0f : 255.0f / 224.0f;

    transform.redV = 2.0f * (1.0f - kr);
    transform.greenU = 2.0f * kb * (1.0f - kb) / kg;
    transform.greenV = 2.0f * kr * (1.0f - kr) / kg;
    transform.blueU = 2.0f * (1.0f - kb);

    return transform;
}
} // namespace

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

Graphics::Rect CameraView::computeImageArea(float viewWidth,
                                            float viewHeight,
                                            int textureWidth,
                                            int textureHeight,
                                            Fit fit,
                                            int rotationDegrees)
{
    auto turned = isQuarterTurn(rotationDegrees);
    auto shownWidth = turned ? textureHeight : textureWidth;
    auto shownHeight = turned ? textureWidth : textureHeight;

    return Sprites::fitRect(viewWidth, viewHeight, shownWidth, shownHeight, fit);
}

Graphics::Rect CameraView::imageAreaFor(int textureWidth,
                                        int textureHeight,
                                        int rotationDegrees) const
{
    auto bounds = getLocalBounds();
    return computeImageArea(
        bounds.w, bounds.h, textureWidth, textureHeight, fit, rotationDegrees);
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
        imageArea = imageAreaFor(texture.width(), texture.height(), 0);
        renderer->drawTexture(texture,
                              imageArea,
                              mirrored,
                              false,
                              Graphics::Color::white(),
                              frameSampling);
        drew = true;
    }

    // The wrapped texture holds its own reference to the frame's surface, so the
    // buffer can be released now.
    Camera::releasePixelBuffer(buffer);
    return drew;
}

bool CameraView::uploadLatestFrame()
{
    if (!camera->copyLatestFrame(scratch))
        return true;

    auto isNv12 = scratch.format == PixelFormat::NV12;

    auto rebuild = !uploadTexture.has_value()
                   || uploadTexture->width() != scratch.width
                   || uploadTexture->height() != scratch.height
                   || uploadedFormat != scratch.format;

    if (rebuild)
    {
        auto descriptor = GPU::TextureDescriptor {};
        descriptor.width = scratch.width;
        descriptor.height = scratch.height;
        descriptor.format =
            isNv12 ? GPU::TextureFormat::R8Unorm : GPU::TextureFormat::BGRA8Unorm;
        uploadTexture.emplace(GPU::Device::shared().makeTexture(descriptor));

        if (isNv12)
        {
            descriptor.width = scratch.width / 2;
            descriptor.height = scratch.height / 2;
            descriptor.format = GPU::TextureFormat::RG8Unorm;
            chromaTexture.emplace(GPU::Device::shared().makeTexture(descriptor));
        }
        else
        {
            chromaTexture.reset();
        }

        uploadedFormat = scratch.format;
    }

    if (!uploadTexture->isValid()
        || (isNv12 && !(chromaTexture.has_value() && chromaTexture->isValid())))
        return false;

    uploadTexture->update(scratch.data.data());

    if (isNv12)
        chromaTexture->update(scratch.data.data() + scratch.width * scratch.height,
                              scratch.width);

    return true;
}

bool CameraView::renderCpuUpload(Graphics::Rect& imageArea)
{
    if (!uploadLatestFrame() || !uploadTexture.has_value()
        || !uploadTexture->isValid() || scratch.width <= 0)
        return false;

    imageArea = imageAreaFor(scratch.width, scratch.height, scratch.rotationDegrees);
    auto placement = placementFor(imageArea, scratch.rotationDegrees, mirrored);

    if (uploadedFormat != PixelFormat::NV12)
    {
        renderer->drawTextureQuad(*uploadTexture,
                                  placement.origin,
                                  placement.edgeX,
                                  placement.edgeY,
                                  Graphics::Color::white(),
                                  frameSampling);
        return true;
    }

    if (!chromaTexture.has_value() || !chromaTexture->isValid())
        return false;

    renderer->drawNv12Quad(*uploadTexture,
                           *chromaTexture,
                           yuvTransformFor(scratch.yuvMatrix, scratch.fullRangeYuv),
                           placement.origin,
                           placement.edgeX,
                           placement.edgeY,
                           Graphics::Color::white(),
                           frameSampling);
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

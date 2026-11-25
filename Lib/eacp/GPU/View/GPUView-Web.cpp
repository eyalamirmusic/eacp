#include "GPUView.h"

#include "../Device/Device.h"
#include "../Frame/Frame.h"
#include "../Texture/Texture.h"
#include "../WebGPU/WebGPUContext.h"

#include <eacp/Core/Threads/EventLoop.h>
#include <eacp/Core/Utils/Logging.h>
#include <eacp/Graphics/Helpers/DisplayLink.h>
#include <eacp/Graphics/Image/Image.h>
#include <eacp/Graphics/View/View-Linux.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <string_view>

// A presenting view on the web is the window's <canvas> under a WebGPU canvas
// context: the window backend hands its CSS selector over in the view's
// NativeSurfaceHandle (Kind::Web), the context is configured at the canvas's
// pixel size, and each frame renders into the texture the context hands out
// for that turn, which the page shows once the turn returns - there is no
// present call. Frames are paced by the window's requestFrameCallback, which
// the web backend answers on requestAnimationFrame.

namespace eacp::GPU
{
namespace
{
TextureFormat textureFormatOf(wgpu::TextureFormat format)
{
    switch (format)
    {
        case wgpu::TextureFormat::RGBA8Unorm:
            return TextureFormat::RGBA8Unorm;
        case wgpu::TextureFormat::RGBA16Float:
            return TextureFormat::RGBA16Float;
        default:
            return TextureFormat::BGRA8Unorm;
    }
}
} // namespace

struct GPUView::Native
{
    explicit Native(GPUView& viewToUse)
        : view(viewToUse)
        , record(Graphics::requestViewSurface(viewToUse))
    {
        record.onAvailable = [this] { surfaceAvailable(); };
        record.onLost = [this] { surfaceLost(); };
        record.onResized = [this] { surfaceResized(); };
        record.onRepaint = [this] { render(); };
        record.onFrameDone = [this] { frameCallbackArrived(); };
    }

    ~Native()
    {
        record.onAvailable = [] {};
        record.onLost = [] {};
        record.onResized = [] {};
        record.onRepaint = [] {};
        record.onFrameDone = [] {};

        stopContinuous();
        surfaceLost();
    }

    void surfaceAvailable()
    {
        if (!createSurface())
            return;

        configured = false;

        if (continuous)
            continuousTick(true);
        else
            render();
    }

    void surfaceLost()
    {
        if (surface != nullptr)
            surface.Unconfigure();

        surface = nullptr;
        configured = false;
        companions = {};
    }

    void surfaceResized()
    {
        configured = false;
        view.repaint();
    }

    void frameCallbackArrived()
    {
        if (continuous)
            continuousTick(false);
    }

    float surfaceScale() const
    {
        if (record.handle.isValid() && record.scale > 0.f)
            return record.scale;

        return Graphics::linuxDefaultBackingScale;
    }

    void notifyScaleChange()
    {
        if (!record.handle.isValid())
            return;

        const auto scale = surfaceScale();
        const auto changed = backingScale > 0.f && scale != backingScale;
        backingScale = scale;

        if (changed)
            view.onBackingScaleChanged(scale);
    }

    void startContinuous()
    {
        stampedTick = Threads::DisplayLink::timedTick(
            [this](Threads::FrameTime time)
            {
                view.update(time);
                render();
            });

        pacingStarted = false;
        continuousTick(true);
    }

    void stopContinuous()
    {
        stampedTick = [] {};
        pacingStarted = false;
    }

    bool tickIsDue()
    {
        using Clock = std::chrono::steady_clock;

        if (maxFps <= 0)
        {
            pacingStarted = false;
            return true;
        }

        const auto interval = 1.0 / static_cast<double>(maxFps);
        const auto now = Clock::now();

        if (!pacingStarted)
        {
            pacingStarted = true;
            lastTick = now;
            accumulated = interval;
        }

        const auto period = std::chrono::duration<double>(now - lastTick).count();
        lastTick = now;
        accumulated += period;

        if (accumulated + period * 0.5 < interval)
            return false;

        accumulated = std::min(accumulated - interval, interval * 0.5);
        return true;
    }

    void continuousTick(bool force)
    {
        if (!continuous || surface == nullptr)
            return;

        if (!force && !tickIsDue())
        {
            record.requestFrameCallback();
            return;
        }

        stampedTick();
    }

    bool createSurface()
    {
        if (surface != nullptr)
            return true;

        if (record.handle.kind != Graphics::NativeSurfaceHandle::Kind::Web
            || record.handle.surface == nullptr)
            return false;

        auto& context = getWebGPUContext(Device::shared());

        if (!context.isValid())
            return false;

        if (instance == nullptr)
            instance = wgpu::CreateInstance();

        const auto selector =
            std::string_view {static_cast<const char*>(record.handle.surface)};

        auto canvas = wgpu::EmscriptenSurfaceSourceCanvasHTMLSelector {};
        canvas.selector = wgpu::StringView {selector.data(), selector.size()};

        auto descriptor = wgpu::SurfaceDescriptor {};
        descriptor.nextInChain = &canvas;
        surface = instance.CreateSurface(&descriptor);

        return surface != nullptr;
    }

    bool configure()
    {
        if (configured)
            return true;

        if (surface == nullptr || record.pixelWidth <= 0 || record.pixelHeight <= 0)
            return false;

        auto& context = getWebGPUContext(Device::shared());

        // Every canvas takes bgra8unorm, and it is the drawable format a
        // pipeline descriptor defaults to on every backend, so it is used
        // whatever the page would prefer.
        format = wgpu::TextureFormat::BGRA8Unorm;

        auto configuration = wgpu::SurfaceConfiguration {};
        configuration.device = context.getDevice();
        configuration.format = format;
        configuration.usage = wgpu::TextureUsage::RenderAttachment;

        if (readableDrawable)
            configuration.usage |= wgpu::TextureUsage::CopySrc;

        configuration.width = static_cast<std::uint32_t>(record.pixelWidth);
        configuration.height = static_cast<std::uint32_t>(record.pixelHeight);
        configuration.alphaMode = transparent
                                      ? wgpu::CompositeAlphaMode::Premultiplied
                                      : wgpu::CompositeAlphaMode::Opaque;
        surface.Configure(&configuration);

        width = record.pixelWidth;
        height = record.pixelHeight;
        configured = true;
        companionsStale = true;
        notifyScaleChange();
        return true;
    }

    // The multisample image and the depth plane the canvas's texture draws
    // with, made at the canvas's size and kept across frames.
    void rebuildCompanions()
    {
        companionsStale = false;
        companions = {};

        auto& gpu = Device::shared();
        const auto samples = gpu.supportsSampleCount(sampleCount) ? sampleCount : 1;

        if (samples <= 1 && !depthEnabled)
            return;

        auto descriptor = TextureDescriptor {};
        descriptor.width = width;
        descriptor.height = height;
        descriptor.format = textureFormatOf(format);
        descriptor.renderTarget = true;
        descriptor.sampleCount = samples;
        descriptor.depth = depthEnabled;
        descriptor.stencil = stencilEnabled;

        companionTexture.create(gpu, descriptor, nullptr);

        if (companionTexture->isValid())
            companions =
                *static_cast<WebGPUTextureData*>(companionTexture->nativeTexture());
    }

    bool readyToRender()
    {
        if (!record.handle.isValid() || !Device::shared().isValid())
            return false;

        if (!createSurface() || !configure())
            return false;

        if (companionsStale)
            rebuildCompanions();

        return true;
    }

    void render()
    {
        render([this](Frame& frame) { view.render(frame); });
    }

    bool render(const std::function<void(Frame&)>& draw)
    {
        if (rendering || !readyToRender())
            return false;

        rendering = true;
        const auto rendered = renderOneFrame(draw);
        rendering = false;

        return rendered;
    }

    bool renderOneFrame(const std::function<void(Frame&)>& draw)
    {
        auto current = wgpu::SurfaceTexture {};
        surface.GetCurrentTexture(&current);

        if (current.texture == nullptr)
        {
            configured = false;
            return false;
        }

        auto target = WebGPUTextureData {};
        target.texture = current.texture;
        target.format = format;
        target.textureFormat = textureFormatOf(format);
        target.width = width;
        target.height = height;
        target.renderTarget = true;
        target.multisample = companions.multisample;
        target.sampleCount =
            companions.multisample != nullptr ? companions.sampleCount : 1;
        target.depth = companions.depth;
        target.depthFormat = companions.depthFormat;

        auto drawable = WebGPUDrawable {&target};

        record.requestFrameCallback();

        {
            auto frame =
                Frame(Device::shared(), &drawable, nullptr, nullptr, surfaceScale());
            draw(frame);
        }

        return true;
    }

    GPUView& view;
    Graphics::ViewSurface& record;

    int sampleCount = 1;
    int maxFps = 0;
    int framesInFlight = 2;
    bool depthEnabled = false;
    bool readableDrawable = false;
    bool stencilEnabled = false;
    bool continuous = false;
    bool transparent = false;

    wgpu::Instance instance;
    wgpu::Surface surface;
    wgpu::TextureFormat format = wgpu::TextureFormat::BGRA8Unorm;
    int width = 0;
    int height = 0;
    bool configured = false;
    bool companionsStale = true;
    bool rendering = false;

    OwningPointer<Texture> companionTexture;
    WebGPUTextureData companions;

    float backingScale = 0.f;

    Callback stampedTick = [] {};

    std::chrono::steady_clock::time_point lastTick;
    double accumulated = 0.0;
    bool pacingStarted = false;
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
    impl->companionsStale = true;
}

void GPUView::setDepth(bool enabled)
{
    impl->depthEnabled = enabled;

    if (!enabled)
        impl->stencilEnabled = false;

    impl->companionsStale = true;
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

    impl->companionsStale = true;
}

bool GPUView::hasStencil() const
{
    return impl->stencilEnabled;
}

void GPUView::setContinuous(bool continuous)
{
    if (impl->continuous == continuous)
        return;

    impl->continuous = continuous;

    if (continuous)
        impl->startContinuous();
    else
        impl->stopContinuous();
}

bool GPUView::isContinuous() const
{
    return impl->continuous;
}

// The canvas context's usage is fixed when it is configured.
void GPUView::setReadableDrawable(bool readable)
{
    if (impl->readableDrawable == readable)
        return;

    impl->readableDrawable = readable;
    impl->configured = false;
}

bool GPUView::hasReadableDrawable() const
{
    return impl->readableDrawable;
}

void GPUView::setMaxFps(int fps)
{
    impl->maxFps = fps;
    impl->pacingStarted = false;
}

int GPUView::maxFps() const
{
    return impl->maxFps;
}

// The page decides how many frames it queues; kept for the API's sake.
void GPUView::setFramesInFlight(int count)
{
    impl->framesInFlight = count < 1 ? 1 : count;
}

int GPUView::framesInFlight() const
{
    return impl->framesInFlight;
}

void GPUView::setTransparent(bool shouldBeTransparent)
{
    impl->transparent = shouldBeTransparent;
    impl->configured = false;
}

bool GPUView::isTransparent() const
{
    return impl->transparent;
}

void GPUView::resizeStarted()
{
    impl->notifyScaleChange();
}

void GPUView::resizeFinished() {}

float GPUView::backingScale() const
{
    if (renderScale > 0.f)
        return renderScale;

    return impl->surfaceScale();
}

void GPUView::paint(Graphics::Context&) {}

void GPUView::renderNow()
{
    impl->render();
}

bool GPUView::renderFrame(const std::function<void(Frame&)>& draw)
{
    return impl->render(draw);
}

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
    descriptor.format = TextureFormat::BGRA8Unorm;
    descriptor.renderTarget = true;
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

        auto frame = Frame(device, target, scale);

        renderScale = scale;
        render(frame);
        renderScale = 0.f;
    }

    const auto pixelCount =
        static_cast<std::size_t>(pixelWidth) * static_cast<std::size_t>(pixelHeight);

    auto rgba = Vector<std::uint8_t>(static_cast<int>(pixelCount * 4));
    texture.read(rgba.data());

    auto image = Graphics::Image {};
    auto* dst = image.prepareForOverwrite(pixelWidth, pixelHeight);

    if (dst == nullptr)
        return {};

    for (auto pixel = std::size_t {0}; pixel < pixelCount; ++pixel)
    {
        const auto* src = rgba.data() + pixel * 4;
        auto* out = dst + pixel * 4;
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

        out[0] = straight(src[2]);
        out[1] = straight(src[1]);
        out[2] = straight(src[0]);
        out[3] = a;
    }

    return image;
}

bool GPUView::renderNativeContentToTarget(void*, float)
{
    return false;
}
} // namespace eacp::GPU

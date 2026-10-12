#include "Common.h"

#include <cstdint>
#include <functional>

// One render drawing into several views' drawables
// (GPUView::renderFrame from inside another view's render(), a frame nested in
// the open one). The outer frame is a snapshot - renderToImage, which every
// lane can run - and the views nested in it are bare GPUViews that are never
// put in a window: on Metal a CAMetalLayer hands out drawables wherever it is,
// so the nested frames are real drawables there; on Linux and Windows a view
// with no window has no swapchain, renderFrame says so, and the cases check
// only what does not need one.
//
// Every picture here is a clear, so no shader is involved: what is checked is
// where the pixels land and in what order.

using namespace nano;
using namespace eacp;
using namespace eacp::GPU;

namespace
{
constexpr auto viewSize = 4.f;

bool canTest()
{
    return Device::shared().isValid();
}

struct FrameRunner final : GPUView
{
    explicit FrameRunner(int samples = 1) { setSampleCount(samples); }

    void render(Frame& frame) override { draw(frame); }

    std::function<void(Frame&)> draw = [](Frame&) {};
};

Graphics::Image runFrame(std::function<void(Frame&)> draw, int samples = 1)
{
    auto view = FrameRunner {samples};
    view.draw = std::move(draw);
    view.setBounds({0.f, 0.f, viewSize, viewSize});
    return view.renderToImage(1.f);
}

// A bare view, laid out by hand: it is in no window, so nothing else would
// size its drawable.
struct ExtraView
{
    explicit ExtraView(bool readable = true)
    {
        view.setSampleCount(1);
        view.setReadableDrawable(readable);
        view.setBounds({0.f, 0.f, viewSize, viewSize});
        view.resizeStarted();
    }

    GPUView view;
};

Texture makeTexture(TextureFormat format,
                    int width,
                    int height,
                    bool renderTarget = false,
                    int samples = 1)
{
    auto descriptor = TextureDescriptor {};
    descriptor.width = width;
    descriptor.height = height;
    descriptor.format = format;
    descriptor.renderTarget = renderTarget;
    descriptor.sampleCount = samples;
    return Device::shared().makeTexture(descriptor);
}

Texture makeCopyTarget(const Frame& frame)
{
    const auto size = frame.pixelSize();
    return makeTexture(frame.drawableFormat(), (int) size.x, (int) size.y);
}

void clear(Frame& frame, const Graphics::Color& color)
{
    auto pass = frame.beginPass({color});
    pass.end();
}

void clear(Frame& frame, const Texture& target, const Graphics::Color& color)
{
    auto pass = frame.beginPass(target, {color});
    pass.end();
}

// Whether every texel of a BGRA8 (or RGBA8) texture is `color`, within a step.
bool holds(const Texture& texture, const Graphics::Color& color)
{
    if (!texture.isValid())
        return false;

    auto bytes = Vector<std::uint8_t> {};
    bytes.resize((std::size_t) (texture.width() * texture.height() * 4));
    texture.read(bytes.data());

    const auto bgra = texture.format() == TextureFormat::BGRA8Unorm;
    const float wanted[] = {
        bgra ? color.b : color.r, color.g, bgra ? color.r : color.b, color.a};

    for (auto texel = 0; texel < texture.width() * texture.height(); ++texel)
        for (auto channel = 0; channel < 4; ++channel)
        {
            const auto got = (int) bytes[(std::size_t) (texel * 4 + channel)];
            const auto expected = (int) (wanted[channel] * 255.f + 0.5f);

            if (got < expected - 1 || got > expected + 1)
                return false;
        }

    return true;
}

bool imageHolds(const Graphics::Image& image, const Graphics::Color& color)
{
    if (!image.isValid())
        return false;

    const auto pixel = image.at(1, 1);

    const auto near = [](float a, float b)
    { return a > b - 0.01f && a < b + 0.01f; };

    return near(pixel.r, color.r) && near(pixel.g, color.g)
           && near(pixel.b, color.b);
}

const auto slate = Graphics::Color {0.25f, 0.5f, 0.75f, 1.f};
const auto red = Graphics::Color {1.f, 0.f, 0.f, 1.f};
const auto green = Graphics::Color {0.f, 1.f, 0.f, 1.f};
const auto blue = Graphics::Color {0.f, 0.f, 1.f, 1.f};
const auto magenta = Graphics::Color {1.f, 0.f, 1.f, 1.f};
const auto amber = Graphics::Color {1.f, 0.75f, 0.f, 1.f};
} // namespace

auto tMultiViewOneExtra =
    test("MultiView/aFrameRendersIntoAnotherViewsDrawable") = []
{
    if (!canTest())
        return;

    auto extra = ExtraView {};

    auto written = makeTexture(TextureFormat::BGRA8Unorm, 4, 4, true);
    auto drawnInside = makeTexture(TextureFormat::BGRA8Unorm, 4, 4, true);
    auto seenInside = makeTexture(TextureFormat::BGRA8Unorm, 4, 4);
    auto seenAfter = makeTexture(TextureFormat::BGRA8Unorm, 4, 4);
    auto extraCopy = Texture {Device::shared(), {}, nullptr};
    auto outerCopy = Texture {Device::shared(), {}, nullptr};

    auto rendered = false;
    auto nestedIndex = std::uint64_t {0};
    auto outerIndex = std::uint64_t {0};
    auto openInside = false;

    const auto image = runFrame(
        [&](Frame& frame)
        {
            outerIndex = Device::shared().frameIndex();

            // Before the nested frame: a texture it reads.
            clear(frame, written, green);

            rendered = extra.view.renderFrame(
                [&](Frame& nested)
                {
                    nestedIndex = Device::shared().frameIndex();
                    openInside = Device::shared().isFrameOpen();

                    extraCopy = makeCopyTarget(nested);
                    clear(nested, blue);
                    check(nested.copyDrawable(extraCopy));

                    nested.copy(written, seenInside);
                    clear(nested, drawnInside, magenta);
                });

            // After it: a texture it wrote.
            frame.copy(drawnInside, seenAfter);

            outerCopy = makeCopyTarget(frame);
            clear(frame, red);
            check(frame.copyDrawable(outerCopy));
        });

    check(imageHolds(image, red));
    check(holds(outerCopy, red));
    check(!Device::shared().isFrameOpen());

    // Linux and Windows give a view in no window no swapchain to render into.
    if constexpr (Platform::isApple())
        check(rendered);

    if (!rendered)
        return;

    check(openInside);
    check(nestedIndex == outerIndex);
    check(holds(extraCopy, blue));
    check(holds(seenInside, green));
    check(holds(seenAfter, magenta));
};

auto tMultiViewTwoExtras = test("MultiView/oneFrameDrawsIntoTwoMoreViews") = []
{
    if (!canTest())
        return;

    auto first = ExtraView {};
    auto second = ExtraView {};

    auto firstCopy = Texture {Device::shared(), {}, nullptr};
    auto secondCopy = Texture {Device::shared(), {}, nullptr};
    auto renderedFirst = false;
    auto renderedSecond = false;

    const auto image = runFrame(
        [&](Frame& frame)
        {
            clear(frame, slate);

            renderedFirst = first.view.renderFrame(
                [&](Frame& nested)
                {
                    firstCopy = makeCopyTarget(nested);
                    clear(nested, amber);
                    nested.copyDrawable(firstCopy);
                });

            renderedSecond = second.view.renderFrame(
                [&](Frame& nested)
                {
                    secondCopy = makeCopyTarget(nested);
                    clear(nested, magenta);
                    nested.copyDrawable(secondCopy);
                });

            // The outer frame's own drawable kept what it drew before either.
            auto loaded = frame.beginPass({.clear = false});
            loaded.end();
        });

    check(imageHolds(image, slate));

    if constexpr (Platform::isApple())
        check(renderedFirst && renderedSecond);

    if (renderedFirst)
        check(holds(firstCopy, amber));

    if (renderedSecond)
        check(holds(secondCopy, magenta));
};

auto tMultiViewOwnFrame = test("MultiView/aViewDoesNotNestInItsOwnFrame") = []
{
    if (!canTest())
        return;

    auto extra = ExtraView {};
    auto inner = false;

    const auto rendered = extra.view.renderFrame(
        [&](Frame& frame)
        {
            clear(frame, slate);
            inner = extra.view.renderFrame([](Frame&) {});
        });

    check(!inner);

    if constexpr (Platform::isApple())
        check(rendered);

    check(!Device::shared().isFrameOpen());
};

auto tMultiViewUnreadable = test("MultiView/anUnreadableDrawableIsNotCopied") = []
{
    if (!canTest())
        return;

    auto extra = ExtraView {false};
    check(!extra.view.hasReadableDrawable());

    auto copied = true;

    const auto rendered = extra.view.renderFrame(
        [&](Frame& frame)
        {
            auto copy = makeCopyTarget(frame);
            clear(frame, slate);

            // D3D12's back buffers copy whatever the view says.
            copied = frame.copyDrawable(copy);
        });

    if (rendered && !Platform::isWindows())
        check(!copied);
};

#include "Common.h"

#include <cstdint>
#include <functional>

// Frame::copyDrawable: the picture a frame has drawn into its own target so
// far, copied into a texture that outlives the frame - a window's screenshot.
// The frames are snapshots (renderToImage), which every lane can run; the copy
// out of a real drawable is in MultiViewFrameTests, whose nested views have
// one. Every picture is a clear, so no shader is involved.

using namespace nano;
using namespace eacp;
using namespace eacp::GPU;

namespace DrawableCopyTests
{
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
} // namespace

auto tDrawableCopy = test("DrawableCopy/theFramesPictureIsCopiedOut") = []
{
    if (!canTest())
        return;

    for (const auto samples: {1, 4})
    {
        if (!Device::shared().supportsSampleCount(samples))
            continue;

        auto copy = Texture {Device::shared(), {}, nullptr};
        auto copied = false;

        // The copy holds the picture as it was when it was recorded; the pass
        // after it is what the frame presents.
        const auto image = runFrame(
            [&](Frame& frame)
            {
                check(frame.drawableFormat() == TextureFormat::BGRA8Unorm);

                copy = makeCopyTarget(frame);
                clear(frame, slate);
                copied = frame.copyDrawable(copy);
                clear(frame, red);
            },
            samples);

        check(copied);
        check(holds(copy, slate));
        check(imageHolds(image, red));
    }
};

auto tDrawableCopyRefusals =
    test("DrawableCopy/aTextureThatDoesNotFitIsRefused") = []
{
    if (!canTest())
        return;

    runFrame(
        [&](Frame& frame)
        {
            const auto size = frame.pixelSize();
            const auto width = (int) size.x;
            const auto height = (int) size.y;

            clear(frame, slate);

            auto otherFormat =
                makeTexture(TextureFormat::RGBA16Float, width, height);
            auto smaller = makeTexture(frame.drawableFormat(), width - 1, height);
            auto multisampled =
                makeTexture(frame.drawableFormat(), width, height, true, 4);
            auto invalid = Texture {Device::shared(), {}, nullptr};

            check(!frame.copyDrawable(otherFormat));
            check(!frame.copyDrawable(smaller));
            check(!frame.copyDrawable(invalid));

            if (multisampled.isValid())
                check(!frame.copyDrawable(multisampled));
        });
};
} // namespace DrawableCopyTests

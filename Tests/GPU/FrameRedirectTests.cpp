#include "Common.h"

#include <cstdint>
#include <functional>

// Frame::Redirect: a frame's own target pointed at an app-owned texture for the
// length of a scope, so code written against the drawable renders into the
// texture instead, on the same command buffer. The frames are snapshots
// (renderToImage), which every lane can run.

using namespace nano;
using namespace eacp;
using namespace eacp::GPU;

namespace FrameRedirectTests
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
    void render(Frame& frame) override { draw(frame); }

    std::function<void(Frame&)> draw = [](Frame&) {};
};

Graphics::Image runFrame(const std::function<void(Frame&)>& draw)
{
    auto view = FrameRunner {};
    view.setSampleCount(1);
    view.draw = draw;
    view.setBounds({0.f, 0.f, viewSize, viewSize});
    return view.renderToImage(1.f);
}

Texture makeTarget(int width, int height, int samples = 1, bool renderTarget = true)
{
    auto descriptor = TextureDescriptor {};
    descriptor.width = width;
    descriptor.height = height;
    descriptor.format = TextureFormat::BGRA8Unorm;
    descriptor.renderTarget = renderTarget;
    descriptor.sampleCount = samples;
    return Device::shared().makeTexture(descriptor);
}

bool holds(const Texture& texture, const Graphics::Color& color)
{
    if (!texture.isValid())
        return false;

    auto bytes = Vector<std::uint8_t> {};
    bytes.resize((std::size_t) (texture.width() * texture.height() * 4));
    texture.read(bytes.data());

    const float wanted[] = {color.b, color.g, color.r, color.a};

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

void clear(Frame& frame, const Graphics::Color& color)
{
    auto pass = frame.beginPass({color});
    pass.end();
}

const auto slate = Graphics::Color {0.25f, 0.5f, 0.75f, 1.f};
const auto red = Graphics::Color {1.f, 0.f, 0.f, 1.f};
const auto green = Graphics::Color {0.f, 1.f, 0.f, 1.f};
} // namespace

auto tAnswersForTarget = test("FrameRedirect/theFrameAnswersForTheTarget") = []
{
    if (!canTest())
        return;

    auto target = makeTarget(6, 2);

    runFrame(
        [&](Frame& frame)
        {
            check(!frame.isRedirected());
            check(frame.pixelSize().x == viewSize);

            {
                auto redirect = Frame::Redirect {frame, target, 2.f};

                check(frame.isRedirected());
                check(frame.isValid());
                check(frame.pixelSize().x == 6.f && frame.pixelSize().y == 2.f);
                check(frame.backingScale() == 2.f);
                check(frame.logicalSize().x == 3.f && frame.logicalSize().y == 1.f);
                check(frame.drawableFormat() == TextureFormat::BGRA8Unorm);
            }

            check(!frame.isRedirected(), "the scope ended");
            check(frame.pixelSize().x == viewSize && frame.backingScale() == 1.f);
        });

    auto notATarget = makeTarget(2, 2, 1, false);

    runFrame(
        [&](Frame& frame)
        {
            auto redirect = Frame::Redirect {frame, notATarget, 1.f};
            check(!frame.isValid(), "a texture that cannot be drawn into");
        });
};

auto tDefaultPass = test("FrameRedirect/theDefaultPassRendersIntoTheTarget") = []
{
    if (!canTest())
        return;

    auto target = makeTarget(3, 3);

    const auto image = runFrame(
        [&](Frame& frame)
        {
            clear(frame, red);

            auto redirect = Frame::Redirect {frame, target, 1.f};
            clear(frame, slate);
        });

    check(holds(target, slate));
    check(imageHolds(image, red), "and the drawable keeps what it had");
};

auto tNesting = test("FrameRedirect/redirectsNestAndRestore") = []
{
    if (!canTest())
        return;

    auto outer = makeTarget(2, 2);
    auto inner = makeTarget(5, 5);

    runFrame(
        [&](Frame& frame)
        {
            auto first = Frame::Redirect {frame, outer, 1.f};
            clear(frame, red);

            {
                auto second = Frame::Redirect {frame, inner, 3.f};

                check(frame.pixelSize().x == 5.f && frame.backingScale() == 3.f);
                clear(frame, green);
            }

            check(frame.pixelSize().x == 2.f && frame.backingScale() == 1.f,
                  "the outer redirect is back");
            clear(frame, slate);
        });

    check(holds(outer, slate));
    check(holds(inner, green));
};

auto tCopyDrawable = test("FrameRedirect/copyDrawableCopiesTheTarget") = []
{
    if (!canTest())
        return;

    auto target = makeTarget(4, 2);
    auto copy = makeTarget(4, 2, 1, false);
    auto wrongSize = makeTarget(2, 2, 1, false);
    auto copied = false;
    auto refused = true;

    runFrame(
        [&](Frame& frame)
        {
            auto redirect = Frame::Redirect {frame, target, 1.f};

            clear(frame, slate);
            copied = frame.copyDrawable(copy);
            refused = !frame.copyDrawable(wrongSize);
            clear(frame, red);
        });

    check(copied);
    check(refused, "a texture not the target's size is refused");
    check(holds(copy, slate), "the picture as it was when copied");
    check(holds(target, red));
};

auto tMultisampled = test("FrameRedirect/aMultisampledTargetIsResolved") = []
{
    if (!canTest() || !Device::shared().supportsSampleCount(4))
        return;

    auto target = makeTarget(4, 4, 4);
    auto copy = makeTarget(4, 4, 1, false);

    runFrame(
        [&](Frame& frame)
        {
            auto redirect = Frame::Redirect {frame, target, 1.f};

            clear(frame, slate);
            check(frame.copyDrawable(copy));
        });

    check(target.sampleCount() == 4);
    check(holds(target, slate));
    check(holds(copy, slate), "the resolved picture is what is copied");
};

namespace
{
struct QuadVertex
{
    float position[2];
};

struct InstanceColor
{
    float color[3];
};
} // namespace
} // namespace FrameRedirectTests

EACP_SHADER_VALUE(FrameRedirectTests::QuadVertex, Float2)

namespace FrameRedirectTests
{
namespace
{
constexpr QuadVertex fullQuad[] = {
    {{-1.f, -1.f}},
    {{1.f, -1.f}},
    {{-1.f, 1.f}},
    {{1.f, -1.f}},
    {{1.f, 1.f}},
    {{-1.f, 1.f}},
};

struct ColorKernel final : ComputeProgram
{
    ColorKernel() { compile(); }

    void define() override
    {
        auto i = threadId();
        write(output, i, base + toFloat(i) * stepSize);
    }

    Uniform<OutputBuffer> output;
    Uniform<Float> base;
    Uniform<Float> stepSize;

    EACP_SHADER(output, base, stepSize)
};

struct ColorFromBuffer final : ShaderProgram
{
    ColorFromBuffer() { compile(); }

    void define() override
    {
        auto position = vertexInput(&QuadVertex::position);
        auto color = instanceInput(&InstanceColor::color, 1);

        setPosition(float4(position, 0.f, 1.f));
        setFragment(float4(varying(color), 1.f));
    }

    EACP_SHADER()
};
} // namespace

auto tComputeFirst =
    test("FrameRedirect/aComputePassFeedsTheRedirectedPassOnOneFrame") = []
{
    if (!canTest())
        return;

    auto colors =
        Buffer {Device::shared(), nullptr, sizeof(float) * 3, BufferUsage::Storage};

    auto kernel = ColorKernel {};
    kernel.output = colors;
    kernel.base = 0.25f;
    kernel.stepSize = 0.25f;
    kernel.prepare();

    auto draw = ColorFromBuffer {};
    draw.setVertices(fullQuad, 6);
    draw.setInstanceBuffer(1, colors, 1);
    draw.prepare(1);

    auto target = makeTarget(3, 3);

    runFrame(
        [&](Frame& frame)
        {
            auto redirect = Frame::Redirect {frame, target, 1.f};

            {
                auto compute = frame.beginCompute();
                compute.dispatch(kernel, 3);
            }

            auto pass = frame.beginPass({red});
            pass.drawInstanced(draw, 1);
        });

    check(holds(target, slate), "the kernel's colour, drawn from its buffer");
};

auto tDrawableUntouched =
    test("FrameRedirect/theDrawableIsUntouchedOutsideTheScope") = []
{
    if (!canTest())
        return;

    auto target = makeTarget(2, 2);

    const auto image = runFrame(
        [&](Frame& frame)
        {
            {
                auto redirect = Frame::Redirect {frame, target, 1.f};
                clear(frame, green);
            }

            clear(frame, slate);
        });

    check(holds(target, green));
    check(imageHolds(image, slate), "the pass after the scope is the drawable's");
};

auto tTimingLabel = test("FrameRedirect/aTimingLabelTimesThePassesUnderIt") = []
{
    if (!canTest())
        return;

    auto target = makeTarget(2, 2);

    auto draw = [&](Frame& frame)
    {
        {
            auto redirect = Frame::Redirect {frame, target, 1.f, "tile"};

            clear(frame, green);

            auto labelled = frame.beginPass({.clearColor = red, .label = "own"});
            labelled.end();

            {
                auto inner = Frame::Redirect {frame, target, 1.f};
                clear(frame, slate);
            }

            clear(frame, green);
        }

        clear(frame, slate);
    };

    runFrame(draw);
    runFrame(draw);

    if (!Device::shared().supportsPassTimings())
        return;

    const auto& passes = Device::shared().lastFrameTimings().passes;

    check(passes.size() == 3, "an inner redirect with no label times nothing");
    check(passes.size() == 3 && passes[0].label == "tile");
    check(passes.size() == 3 && passes[1].label == "tile/own");
    check(passes.size() == 3 && passes[2].label == "tile", "restored after it");
};
} // namespace FrameRedirectTests

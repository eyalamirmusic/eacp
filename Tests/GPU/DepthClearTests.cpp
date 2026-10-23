#include "Common.h"

#include <array>
#include <cstdint>

// RenderPassDescriptor::clearDepth, checked by drawing what only the value the
// plane was cleared to can decide: a quad at depth 0.5 tested GreaterEqual
// passes against 0 and can never pass against 1, so a green pixel is the clear
// value having arrived. Every case has its mirror at the default 1, which comes
// back the clear colour, so a draw that happened for some other reason cannot
// pass for one.
//
// Each place a pass clears depth is a case of its own: the drawable (through
// View::renderToImage, which is OffscreenTarget's frame), a texture target, and
// a pass taking its plane from another target through depthTarget. The last
// case is reversed Z whole: Mat4::perspectiveReversedZ, a clear to 0 and
// GreaterEqual, keeping the nearer of two quads.
//
// Runs on every backend, and self-skips without a GPU device.

using namespace nano;
using namespace eacp;
using namespace eacp::GPU;
using eacp::Maths::Mat4;
using eacp::Maths::Vec4;

namespace DepthClearTests
{
namespace
{
constexpr auto viewWidth = 8;
constexpr auto viewHeight = 4;

struct ClipVertex
{
    float position[4];
};
} // namespace
} // namespace DepthClearTests

EACP_SHADER_VALUE(DepthClearTests::ClipVertex, Float4)

namespace DepthClearTests
{
namespace
{
using Quad = std::array<ClipVertex, 6>;

Quad quadAt(float depth)
{
    return {{{{-1.f, -1.f, depth, 1.f}},
             {{1.f, -1.f, depth, 1.f}},
             {{-1.f, 1.f, depth, 1.f}},
             {{1.f, -1.f, depth, 1.f}},
             {{1.f, 1.f, depth, 1.f}},
             {{-1.f, 1.f, depth, 1.f}}}};
}

// A square facing the camera at `distance`, projected on the CPU so the
// vertices are clip space as the matrix leaves it, w and all.
Quad projectedQuad(const Mat4& projection, float distance)
{
    auto corner = [&](float x, float y)
    {
        const auto clip =
            projection * Vec4 {x * distance, y * distance, -distance, 1.f};
        return ClipVertex {{clip.x, clip.y, clip.z, clip.w}};
    };

    return {corner(-1.f, -1.f),
            corner(1.f, -1.f),
            corner(-1.f, 1.f),
            corner(1.f, -1.f),
            corner(1.f, 1.f),
            corner(-1.f, 1.f)};
}

struct ClipShader final : ShaderProgram
{
    ClipShader() { compile(); }

    void define() override
    {
        setPosition(vertexInput(&ClipVertex::position));
        setFragment(color);
    }

    Uniform<Float4> color;

    EACP_SHADER(color)
};

struct Clear
{
    float depth = 1.f;
    DepthCompare compare = DepthCompare::GreaterEqual;
    DepthAction action = DepthAction::Clear;
};

RenderPipelineDescriptor describe(DepthCompare compare, PixelFormat format)
{
    auto descriptor = RenderPipelineDescriptor {};
    descriptor.sampleCount = 1;
    descriptor.depth = true;
    descriptor.depthCompare = compare;
    descriptor.colorFormat = format;
    return descriptor;
}

RenderPassDescriptor passFor(const Clear& clear)
{
    auto descriptor = RenderPassDescriptor {};
    descriptor.clearColor = Graphics::Color::black();
    descriptor.clearDepth = clear.depth;
    descriptor.depthAction = clear.action;
    return descriptor;
}

void prepareQuad(ClipShader& shader,
                 const Quad& quad,
                 const RenderPipelineDescriptor& descriptor,
                 const Array<float, 4>& color)
{
    shader.color = color;
    shader.setVertices(quad.data(), 6);
    shader.prepare(descriptor);
}

bool isGreen(const Graphics::Color& c)
{
    return c.g > 0.5f && c.r < 0.5f && c.b < 0.5f;
}

bool isRed(const Graphics::Color& c)
{
    return c.r > 0.5f && c.g < 0.5f && c.b < 0.5f;
}

bool isBlack(const Graphics::Color& c)
{
    return c.r < 0.5f && c.g < 0.5f && c.b < 0.5f;
}

Graphics::Color centreOf(GPUView& view)
{
    view.setBounds({0.f, 0.f, (float) viewWidth, (float) viewHeight});

    auto image = view.renderToImage(1.f);
    check(image.isValid());

    return image.at(viewWidth / 2, viewHeight / 2);
}

// One green quad at `quadDepth` into the drawable.
struct DrawableView final : GPUView
{
    DrawableView(const Clear& clearToUse, float quadDepth)
        : clear(clearToUse)
        , quad(quadAt(quadDepth))
    {
        setSampleCount(1);
        setDepth(true);

        prepareQuad(shader,
                    quad,
                    describe(clear.compare, PixelFormat::BGRA8Unorm),
                    {0.f, 1.f, 0.f, 1.f});
    }

    void render(Frame& frame) override
    {
        auto pass = frame.beginPass(passFor(clear));
        pass.draw(shader);
    }

    Clear clear;
    Quad quad;
    ClipShader shader;
};

TextureDescriptor targetDescriptor(bool withDepth)
{
    auto descriptor = TextureDescriptor {};
    descriptor.width = viewWidth;
    descriptor.height = viewHeight;
    descriptor.format = TextureFormat::RGBA8Unorm;
    descriptor.renderTarget = true;
    descriptor.depth = withDepth;
    return descriptor;
}

// The same quad into a texture target, read back once the pass has been
// flushed. With `borrowDepth` the colour target has no plane of its own and
// the pass takes another target's through depthTarget.
struct TargetView final : GPUView
{
    TargetView(const Clear& clearToUse, bool borrowDepthToUse)
        : clear(clearToUse)
        , borrowDepth(borrowDepthToUse)
        , colour(Device::shared().makeTexture(targetDescriptor(!borrowDepth)))
        , depthOwner(Device::shared().makeTexture(targetDescriptor(true)))
        , quad(quadAt(0.5f))
    {
        setSampleCount(1);

        prepareQuad(
            shader,
            quad,
            describe(clear.compare, pixelFormatFor(TextureFormat::RGBA8Unorm)),
            {0.f, 1.f, 0.f, 1.f});
    }

    void render(Frame& frame) override
    {
        auto descriptor = passFor(clear);

        if (borrowDepth)
        {
            descriptor.depthTarget = {&depthOwner, 0, 0};

            const auto target = ColorTarget {&colour};
            auto pass =
                frame.beginMultiTargetPass(std::span {&target, 1}, descriptor);
            pass.draw(shader);
        }
        else
        {
            auto pass = frame.beginPass(colour, descriptor);
            pass.draw(shader);
        }

        frame.flush();
        colour.read(pixels.data());

        auto drawable = frame.beginPass();
    }

    Graphics::Color centre()
    {
        setBounds({0.f, 0.f, (float) viewWidth, (float) viewHeight});
        check(renderToImage(1.f).isValid());

        const auto* texel =
            &pixels[((viewHeight / 2) * viewWidth + viewWidth / 2) * 4];
        return Graphics::Color {
            texel[0] / 255.f, texel[1] / 255.f, texel[2] / 255.f, texel[3] / 255.f};
    }

    Clear clear;
    bool borrowDepth = false;
    Texture colour;
    Texture depthOwner;
    Quad quad;
    ClipShader shader;
    std::array<std::uint8_t, viewWidth * viewHeight * 4> pixels {};
};

// Two quads two and five units from a reversed-Z camera, the nearer drawn
// first, so the farther one is only kept out by the depth test.
struct ReversedZView final : GPUView
{
    explicit ReversedZView(float clearDepthToUse)
        : clearDepth(clearDepthToUse)
        , projection(Mat4::perspectiveReversedZ(2.f, 1.f, 0.5f))
        , nearQuad(projectedQuad(projection, 2.f))
        , farQuad(projectedQuad(projection, 5.f))
    {
        setSampleCount(1);
        setDepth(true);

        const auto descriptor =
            describe(DepthCompare::GreaterEqual, PixelFormat::BGRA8Unorm);

        prepareQuad(nearShader, nearQuad, descriptor, {0.f, 1.f, 0.f, 1.f});
        prepareQuad(farShader, farQuad, descriptor, {1.f, 0.f, 0.f, 1.f});
    }

    void render(Frame& frame) override
    {
        auto descriptor = RenderPassDescriptor {};
        descriptor.clearColor = Graphics::Color::black();
        descriptor.clearDepth = clearDepth;

        auto pass = frame.beginPass(descriptor);
        pass.draw(nearShader);
        pass.draw(farShader);
    }

    float clearDepth = 0.f;
    Mat4 projection;
    Quad nearQuad;
    Quad farQuad;
    ClipShader nearShader;
    ClipShader farShader;
};

Graphics::Color drawn(const Clear& clear, float quadDepth = 0.5f)
{
    auto view = DrawableView {clear, quadDepth};
    check(view.hasDepth());
    return centreOf(view);
}
} // namespace

// The case reversed Z needs: cleared to 0, a GreaterEqual draw passes, and
// against the default 1 the same draw cannot.
auto tDrawableClearsToZero = test("DepthClear/theDrawableClearsToTheValueGiven") = []
{
    if (!Device::shared().isValid())
        return;

    check(isGreen(drawn({0.f})));
    check(isBlack(drawn({1.f})));
};

// Keep clears on entry as Clear does, so it reads the value too.
auto tKeepClearsToo = test("DepthClear/aKeepingPassClearsToTheValueGiven") = []
{
    if (!Device::shared().isValid())
        return;

    check(isGreen(drawn({0.f, DepthCompare::GreaterEqual, DepthAction::Keep})));
    check(isBlack(drawn({1.f, DepthCompare::GreaterEqual, DepthAction::Keep})));
};

// Not only the two ends: cleared to 0.4, Less keeps 0.3 and rejects 0.5.
auto tAValueBetween = test("DepthClear/anyValueInTheRangeIsTheStartingDepth") = []
{
    if (!Device::shared().isValid())
        return;

    check(isGreen(drawn({0.4f, DepthCompare::Less}, 0.3f)));
    check(isBlack(drawn({0.4f, DepthCompare::Less}, 0.5f)));
};

auto tTargetClears = test("DepthClear/aTextureTargetClearsToTheValueGiven") = []
{
    if (!Device::shared().isValid())
        return;

    auto cleared = TargetView {{0.f}, false};
    check(cleared.colour.hasDepth());
    check(isGreen(cleared.centre()));

    auto mirror = TargetView {{1.f}, false};
    check(isBlack(mirror.centre()));
};

auto tDepthTargetClears =
    test("DepthClear/aBorrowedDepthTargetClearsToTheValueGiven") = []
{
    if (!Device::shared().isValid())
        return;

    auto cleared = TargetView {{0.f}, true};
    check(!cleared.colour.hasDepth());
    check(isGreen(cleared.centre()));

    auto mirror = TargetView {{1.f}, true};
    check(isBlack(mirror.centre()));
};

// Reversed Z end to end: the nearer quad stands although the farther one was
// drawn over it, and with the plane cleared to 1 instead nothing draws at all.
auto tReversedZ = test("DepthClear/reversedZKeepsTheNearerQuad") = []
{
    if (!Device::shared().isValid())
        return;

    auto view = ReversedZView {0.f};
    check(isGreen(centreOf(view)));

    auto mirror = ReversedZView {1.f};
    const auto colour = centreOf(mirror);
    check(isBlack(colour) && !isRed(colour));
};
} // namespace DepthClearTests

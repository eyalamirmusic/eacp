#include "Common.h"

#include <cmath>
#include <functional>

// The fragment built-ins the shader graph gained beside fragmentCoordinate():
// frontFacing(), setFragmentDepth(), and the multisample trio sampleIndex(),
// sampleMask() and setSampleMask(). Each is drawn and read back, so what is
// pinned is what the hardware did with the number rather than the spelling the
// emitter chose for it - that half is in StageBuiltInCodegenTests.
//
// frontFacing is two quads of opposite winding side by side, under no culling.
// The depth output is observed through the depth test: a quad whose depth the
// fragment rewrites to 0.25 on the left and 0.75 on the right, then a second
// quad at 0.5 over it, which survives only where the rewritten depth was the
// farther. The sample cases draw into a four-sample target and read the
// resolve: a mask keeping two samples of four gives half the colour, a colour
// only sample 0 gets gives a quarter, and a full quad sees all four samples
// covered everywhere but along the diagonal its two triangles share, where each
// triangle is handed only the samples on its own side.
//
// Runs on every backend, and self-skips without a GPU device - or, for the
// sample cases, without four samples.

using namespace nano;
using namespace eacp;
using namespace eacp::GPU;

namespace FragmentBuiltInTests
{
namespace
{
constexpr auto targetSize = 4;
constexpr auto pixelCount = targetSize * targetSize;
constexpr auto samples = 4;

struct QuadVertex
{
    float position[2];
};
} // namespace
} // namespace FragmentBuiltInTests

EACP_SHADER_VALUE(FragmentBuiltInTests::QuadVertex, Float2)

namespace FragmentBuiltInTests
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

// The left half counter-clockwise in clip space - the front face by eacp's
// convention - and the right half clockwise.
constexpr QuadVertex opposedHalves[] = {
    {{-1.f, -1.f}},
    {{0.f, -1.f}},
    {{-1.f, 1.f}},
    {{0.f, -1.f}},
    {{0.f, 1.f}},
    {{-1.f, 1.f}},
    {{0.f, -1.f}},
    {{0.f, 1.f}},
    {{1.f, -1.f}},
    {{1.f, -1.f}},
    {{0.f, 1.f}},
    {{1.f, 1.f}},
};

struct FrontFacingShader final : ShaderProgram
{
    FrontFacingShader() { compile(); }

    void define() override
    {
        auto position = vertexInput(&QuadVertex::position);

        setPosition(float4(position, 0.f, 1.f));
        setFragment(float4(select(frontFacing(), 1.f, 0.f), 0.f, 0.f, 1.f));
    }

    EACP_SHADER()
};

// Drawn at 0.9, written at 0.25 left of the middle and 0.75 right of it.
struct DepthWritingShader final : ShaderProgram
{
    DepthWritingShader() { compile(); }

    void define() override
    {
        auto position = vertexInput(&QuadVertex::position);

        setPosition(float4(position, 0.9f, 1.f));
        setFragment(float4(constant(0.f), 0.f, 0.f, 1.f));

        auto onTheLeft = fragmentCoordinate().x() < (float) targetSize / 2.f;
        setFragmentDepth(select(onTheLeft, 0.25f, 0.75f));
    }

    EACP_SHADER()
};

struct FlatQuadShader final : ShaderProgram
{
    FlatQuadShader() { compile(); }

    void define() override
    {
        auto position = vertexInput(&QuadVertex::position);

        setPosition(float4(position, 0.5f, 1.f));
        setFragment(float4(constant(1.f), 0.f, 0.f, 1.f));
    }

    EACP_SHADER()
};

// White into samples 0 and 1 only.
struct HalfMaskShader final : ShaderProgram
{
    HalfMaskShader() { compile(); }

    void define() override
    {
        auto position = vertexInput(&QuadVertex::position);

        setPosition(float4(position, 0.f, 1.f));
        setFragment(float4(constant(1.f), 1.f, 1.f, 1.f));
        setSampleMask(unsignedInteger(0b0011u));
    }

    EACP_SHADER()
};

// The same two samples taken from the mask the fragment was handed, so the
// input and the output share one entry point. The two triangles of a quad
// partition every pixel's samples, so each pixel still ends up half white.
struct CoverageFilterShader final : ShaderProgram
{
    CoverageFilterShader() { compile(); }

    void define() override
    {
        auto position = vertexInput(&QuadVertex::position);

        setPosition(float4(position, 0.f, 1.f));
        setFragment(float4(constant(1.f), 1.f, 1.f, 1.f));
        setSampleMask(sampleMask() & 0b0011u);
    }

    EACP_SHADER()
};

// White in sample 0, black in the other three.
struct FirstSampleShader final : ShaderProgram
{
    FirstSampleShader() { compile(); }

    void define() override
    {
        auto position = vertexInput(&QuadVertex::position);
        setPosition(float4(position, 0.f, 1.f));

        auto first = toFloat(sampleIndex() == 0u);
        setFragment(float4(first, first, first, 1.f));
    }

    EACP_SHADER()
};

// White where the triangle covers every sample of the pixel.
struct FullCoverageShader final : ShaderProgram
{
    FullCoverageShader() { compile(); }

    void define() override
    {
        auto position = vertexInput(&QuadVertex::position);
        setPosition(float4(position, 0.f, 1.f));

        auto covered = toFloat(sampleMask() == 0b1111u);
        setFragment(float4(covered, covered, covered, 1.f));
    }

    EACP_SHADER()
};

TextureDescriptor describeFloatTarget(bool depth)
{
    auto descriptor = TextureDescriptor {};
    descriptor.width = targetSize;
    descriptor.height = targetSize;
    descriptor.format = TextureFormat::R32Float;
    descriptor.renderTarget = true;
    descriptor.depth = depth;
    return descriptor;
}

TextureDescriptor describeMultisampledTarget()
{
    auto descriptor = TextureDescriptor {};
    descriptor.width = targetSize;
    descriptor.height = targetSize;
    descriptor.format = TextureFormat::RGBA8Unorm;
    descriptor.renderTarget = true;
    descriptor.sampleCount = samples;
    return descriptor;
}

RenderPipelineDescriptor describeFloatPipeline(bool depth)
{
    auto descriptor = RenderPipelineDescriptor {};
    descriptor.sampleCount = 1;
    descriptor.colorFormat = pixelFormatFor(TextureFormat::R32Float);
    descriptor.depth = depth;
    descriptor.depthCompare = DepthCompare::Less;
    return descriptor;
}

RenderPipelineDescriptor describeMultisampledPipeline()
{
    auto descriptor = RenderPipelineDescriptor {};
    descriptor.sampleCount = samples;
    descriptor.colorFormat = pixelFormatFor(TextureFormat::RGBA8Unorm);
    return descriptor;
}

struct FrameRunner final : GPUView
{
    FrameRunner() { setSampleCount(1); }

    void render(Frame& frame) override
    {
        draw(frame);
        frame.beginPass({Graphics::Color::black()});
    }

    std::function<void(Frame&)> draw = [](Frame&) {};
};

// One pass into `target`, cleared to `clear`, read back before the frame ends
// into `pixels`.
void drawAndRead(Texture& target,
                 const Graphics::Color& clear,
                 void* pixels,
                 const std::function<void(RenderPass&)>& draw)
{
    auto view = FrameRunner {};

    auto drawFrame = [&](Frame& frame)
    {
        {
            auto descriptor = RenderPassDescriptor {};
            descriptor.clearColor = clear;

            auto pass = frame.beginPass(target, descriptor);
            draw(pass);
        }

        frame.flush();
        target.read(pixels);
    };

    view.draw = drawFrame;
    view.setBounds({0.f, 0.f, (float) targetSize, (float) targetSize});
    check(view.renderToImage(1.f).isValid());
}

using FloatPixels = Array<float, pixelCount>;
using BytePixels = Array<unsigned char, pixelCount * 4>;

bool near(float value, float expected)
{
    return std::abs(value - expected) < 0.002f;
}

// Every pixel's red within two steps of `expected` out of 255.
bool everyPixelNear(const BytePixels& pixels, float expected)
{
    for (auto i = 0; i < pixelCount; ++i)
        if (std::abs((float) pixels[i * 4] - expected * 255.f) > 2.f)
            return false;

    return true;
}

bool canTest()
{
    return Device::shared().isValid();
}

bool canMultisample()
{
    return canTest() && Device::shared().supportsSampleCount(samples);
}

template <typename Program>
BytePixels drawMultisampled(Program& program)
{
    program.setVertices(fullQuad);
    program.prepare(describeMultisampledPipeline());

    auto target = Device::shared().makeTexture(describeMultisampledTarget());
    auto pixels = BytePixels {};

    auto draw = [&](RenderPass& pass) { pass.draw(program); };
    drawAndRead(target, Graphics::Color::black(), pixels.data(), draw);
    return pixels;
}
} // namespace

auto tFrontFacing = test("FragmentBuiltIn/frontFacingTellsTheTwoWindingsApart") = []
{
    if (!canTest())
        return;

    auto program = FrontFacingShader {};
    program.setVertices(opposedHalves);
    program.prepare(describeFloatPipeline(false));

    auto target = Device::shared().makeTexture(describeFloatTarget(false));
    auto values = FloatPixels {};

    auto draw = [&](RenderPass& pass) { pass.draw(program); };
    drawAndRead(target, {-1.f, 0.f, 0.f, 1.f}, values.data(), draw);

    for (auto i = 0; i < pixelCount; ++i)
        check(near(values[i], i % targetSize < targetSize / 2 ? 1.f : 0.f));
};

auto tFragmentDepth =
    test("FragmentBuiltIn/fragmentDepthIsWhatTheDepthTestSees") = []
{
    if (!canTest())
        return;

    auto writer = DepthWritingShader {};
    writer.setVertices(fullQuad);
    writer.prepare(describeFloatPipeline(true));

    auto over = FlatQuadShader {};
    over.setVertices(fullQuad);
    over.prepare(describeFloatPipeline(true));

    auto target = Device::shared().makeTexture(describeFloatTarget(true));
    auto values = FloatPixels {};

    auto draw = [&](RenderPass& pass)
    {
        pass.draw(writer);
        pass.draw(over);
    };

    drawAndRead(target, {-1.f, 0.f, 0.f, 1.f}, values.data(), draw);

    for (auto i = 0; i < pixelCount; ++i)
        check(near(values[i], i % targetSize < targetSize / 2 ? 0.f : 1.f));
};

auto tSampleMaskOut = test("FragmentBuiltIn/aSampleMaskKeepsOnlyItsSamples") = []
{
    if (!canMultisample())
        return;

    auto program = HalfMaskShader {};
    check(everyPixelNear(drawMultisampled(program), 0.5f));
};

auto tSampleMaskInAndOut =
    test("FragmentBuiltIn/aMaskTakenFromTheInputMaskFiltersIt") = []
{
    if (!canMultisample())
        return;

    auto program = CoverageFilterShader {};
    check(everyPixelNear(drawMultisampled(program), 0.5f));
};

auto tSampleIndex = test("FragmentBuiltIn/sampleIndexShadesEachSample") = []
{
    if (!canMultisample())
        return;

    auto program = FirstSampleShader {};
    check(everyPixelNear(drawMultisampled(program), 0.25f));
};

auto tSampleMaskIn = test("FragmentBuiltIn/aCoveredPixelSeesEverySample") = []
{
    if (!canMultisample())
        return;

    auto program = FullCoverageShader {};
    auto pixels = drawMultisampled(program);

    for (auto y = 0; y < targetSize; ++y)
    {
        for (auto x = 0; x < targetSize; ++x)
        {
            auto red = pixels[(y * targetSize + x) * 4];

            if (x == y)
                check(red < 250);
            else
                check(red > 252);
        }
    }
};
} // namespace FragmentBuiltInTests

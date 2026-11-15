#include "Common.h"

// The shader graph's array and comparison reads of a depth plane:
// ShaderBuilder::depthTextureArray over the whole plane of a TwoDArray target,
// bound with RenderPass::setFragmentDepthTexture, and sampleCompare through a
// sampling declared with TextureCompare::LessEqual - the cascades of a shadow
// map in one bind, read with the hardware's comparison instead of a sample and
// a test per tap.
//
// Layer 0 holds 0.75 everywhere, layer 1 holds 0.25 on its left half and the
// far plane on its right, so a read of the wrong layer, or of a comparison
// against the wrong side, lands on a number no right answer has.
//
// Runs on every backend, and self-skips without a GPU device.

using namespace nano;
using namespace eacp;
using namespace eacp::GPU;

namespace ShadowSamplingTests
{
namespace
{
constexpr auto targetSize = 4;
constexpr auto pixelCount = targetSize * targetSize;

constexpr auto backLayerDepth = 0.75f;
constexpr auto frontLayerDepth = 0.25f;
constexpr auto farPlane = 1.f;

struct QuadVertex
{
    float position[2];
    float uv[2];
};
} // namespace
} // namespace ShadowSamplingTests

EACP_SHADER_VALUE(ShadowSamplingTests::QuadVertex, Float2)

namespace ShadowSamplingTests
{
namespace
{
constexpr QuadVertex fullQuad[] = {
    {{-1.f, -1.f}, {0.f, 1.f}},
    {{1.f, -1.f}, {1.f, 1.f}},
    {{-1.f, 1.f}, {0.f, 0.f}},
    {{1.f, -1.f}, {1.f, 1.f}},
    {{1.f, 1.f}, {1.f, 0.f}},
    {{-1.f, 1.f}, {0.f, 0.f}},
};

constexpr QuadVertex leftHalf[] = {
    {{-1.f, -1.f}, {0.f, 1.f}},
    {{0.f, -1.f}, {0.5f, 1.f}},
    {{-1.f, 1.f}, {0.f, 0.f}},
    {{0.f, -1.f}, {0.5f, 1.f}},
    {{0.f, 1.f}, {0.5f, 0.f}},
    {{-1.f, 1.f}, {0.f, 0.f}},
};

constexpr auto shadowNearest = TextureSampling {
    TextureFilter::Nearest, TextureAddressMode::Clamp, TextureCompare::LessEqual};

constexpr auto shadowLinear = TextureSampling {
    TextureFilter::Linear, TextureAddressMode::Clamp, TextureCompare::LessEqual};

struct DepthWriter final : ShaderProgram
{
    DepthWriter() { compile(); }

    void define() override
    {
        auto position = vertexInput(&QuadVertex::position);

        setPosition(float4(position, depth, 1.f));
        setFragment(float4(depth, 0.f, 0.f, 1.f));
    }

    Uniform<Float> depth;

    EACP_SHADER(depth)
};

struct LayerReader final : ShaderProgram
{
    LayerReader() { compile(); }

    void define() override
    {
        auto position = vertexInput(&QuadVertex::position);
        auto uv = vertexInput(&QuadVertex::uv);

        setPosition(float4(position, 0.f, 1.f));

        auto depth = sample(cascades, varying(uv), layer);

        setFragment(float4(depth, depth, depth, 1.f));
    }

    Uniform<TextureDepth2DArray> cascades;
    Uniform<Float> layer;

    EACP_SHADER(cascades, layer)
};

// At the texel the pixel covers, or at one fixed coordinate when `atPoint` is
// set - which is how a single lookup is aimed at the boundary between texels.
struct LayerCompareReader final : ShaderProgram
{
    explicit LayerCompareReader(TextureSampling sampling)
    {
        cascades.sampling = sampling;
        compile();
    }

    void define() override
    {
        auto position = vertexInput(&QuadVertex::position);
        auto uv = varying(vertexInput(&QuadVertex::uv));

        setPosition(float4(position, 0.f, 1.f));

        auto at = uv * (1.f - atPoint) + point * atPoint;
        auto lit = sampleCompare(cascades, at, layer, reference);

        setFragment(float4(lit, lit, lit, 1.f));
    }

    Uniform<TextureDepth2DArray> cascades;
    Uniform<Int> layer;
    Uniform<Float> reference;
    Uniform<Float2> point;
    Uniform<Float> atPoint;

    EACP_SHADER(cascades, layer, reference, point, atPoint)
};

struct PlaneCompareReader final : ShaderProgram
{
    PlaneCompareReader()
    {
        shadow.sampling = shadowNearest;
        compile();
    }

    void define() override
    {
        auto position = vertexInput(&QuadVertex::position);
        auto uv = vertexInput(&QuadVertex::uv);

        setPosition(float4(position, 0.f, 1.f));

        auto lit = sampleCompare(shadow, varying(uv), reference);

        setFragment(float4(lit, lit, lit, 1.f));
    }

    Uniform<TextureDepth2D> shadow;
    Uniform<Float> reference;

    EACP_SHADER(shadow, reference)
};

TextureDescriptor describeScene(TextureType kind, int layers)
{
    auto descriptor = TextureDescriptor {};
    descriptor.width = targetSize;
    descriptor.height = targetSize;
    descriptor.kind = kind;
    descriptor.arrayLength = layers;
    descriptor.renderTarget = true;
    descriptor.sampleableDepth = true;
    return descriptor;
}

TextureDescriptor describeFloatTarget()
{
    auto descriptor = TextureDescriptor {};
    descriptor.width = targetSize;
    descriptor.height = targetSize;
    descriptor.format = TextureFormat::R32Float;
    descriptor.renderTarget = true;
    return descriptor;
}

template <typename Program>
void prepareReader(Program& program)
{
    program.setVertices(fullQuad, 6);
    program.prepare(1,
                    false,
                    PrimitiveTopology::Triangles,
                    BlendMode::None,
                    pixelFormatFor(TextureFormat::R32Float));
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

void runFrame(std::function<void(Frame&)> draw)
{
    auto view = FrameRunner {};
    view.draw = std::move(draw);
    view.setBounds({0.f, 0.f, 4.f, 4.f});
    check(view.renderToImage(1.f).isValid());
}

// The two layers the file is about: the back one covered whole, the front one
// on its left half only, each pass keeping what it wrote.
struct Scene
{
    Scene()
        : layers(
              Device::shared().makeTexture(describeScene(TextureType::TwoDArray, 2)))
        , plane(Device::shared().makeTexture(describeScene(TextureType::TwoD, 1)))
    {
        back.setVertices(fullQuad, 6);
        back.depth = backLayerDepth;

        front.setVertices(leftHalf, 6);
        front.depth = frontLayerDepth;

        for (auto* writer: {&back, &front})
            writer->prepare(1,
                            true,
                            PrimitiveTopology::Triangles,
                            BlendMode::None,
                            pixelFormatFor(TextureFormat::RGBA8Unorm));
    }

    bool isValid() const
    {
        return layers.hasSampleableDepth() && plane.hasSampleableDepth();
    }

    void draw(Frame& frame)
    {
        drawLayer(frame, layers, 0, back);
        drawLayer(frame, layers, 1, front);
        drawLayer(frame, plane, 0, front);
    }

    static void drawLayer(Frame& frame,
                          const Texture& target,
                          int slice,
                          DepthWriter& writer)
    {
        auto colour = ColorTarget {&target};
        colour.slice = slice;

        auto descriptor = RenderPassDescriptor {};
        descriptor.depthAction = DepthAction::Keep;

        auto pass = frame.beginMultiTargetPass(
            std::span<const ColorTarget> {&colour, 1}, descriptor);
        pass.draw(writer);
    }

    Texture layers;
    Texture plane;
    DepthWriter back;
    DepthWriter front;
};

// The scene drawn, then `reader` into a float target cleared to a value no
// depth or comparison could give, read back before the frame ends.
template <typename Program>
Array<float, pixelCount> readThrough(Scene& scene, Program& reader)
{
    auto copy = Device::shared().makeTexture(describeFloatTarget());
    auto values = Array<float, pixelCount> {};

    runFrame(
        [&](Frame& frame)
        {
            scene.draw(frame);

            {
                auto descriptor = RenderPassDescriptor {};
                descriptor.clearColor = {-1.f, 0.f, 0.f, 1.f};

                auto into = frame.beginPass(copy, descriptor);
                into.draw(reader);
            }

            frame.flush();
            copy.read(values.data());
        });

    return values;
}

bool near(float value, float expected, float tolerance = 0.002f)
{
    return std::abs(value - expected) < tolerance;
}

bool isLeft(int index)
{
    return index % targetSize < targetSize / 2;
}

bool holdsHalves(const Array<float, pixelCount>& values, float left, float right)
{
    for (auto i = 0; i < pixelCount; ++i)
        if (!near(values[i], isLeft(i) ? left : right))
            return false;

    return true;
}

bool canTest()
{
    return Device::shared().isValid();
}
} // namespace

auto tEachLayerReadsAsWritten =
    test("ShadowSampling/aDepthArrayReadsEachLayerItWasDrawnInto") = []
{
    if (!canTest())
        return;

    auto scene = Scene {};
    check(scene.isValid());

    if (!scene.isValid())
        return;

    auto reader = LayerReader {};
    reader.cascades = scene.layers;
    prepareReader(reader);

    reader.layer = 0.f;
    check(holdsHalves(readThrough(scene, reader), backLayerDepth, backLayerDepth));

    reader.layer = 1.f;
    check(holdsHalves(readThrough(scene, reader), frontLayerDepth, farPlane));
};

// LessEqual with the receiver's depth as the reference: a receiver in front of
// what the layer stored is lit, one behind it is not, and the far plane lights
// everything.
auto tCompareAgainstALayer =
    test("ShadowSampling/aComparisonIsOneInFrontOfTheStoredDepthAndZeroBehind") = []
{
    if (!canTest())
        return;

    auto scene = Scene {};

    if (!scene.isValid())
        return;

    auto reader = LayerCompareReader {shadowNearest};
    reader.cascades = scene.layers;
    reader.layer = 1;
    prepareReader(reader);

    reader.reference = frontLayerDepth - 0.05f;
    check(holdsHalves(readThrough(scene, reader), 1.f, 1.f));

    reader.reference = 0.5f;
    check(holdsHalves(readThrough(scene, reader), 0.f, 1.f));

    reader.layer = 0;
    check(holdsHalves(readThrough(scene, reader), 1.f, 1.f));

    reader.reference = 0.9f;
    check(holdsHalves(readThrough(scene, reader), 0.f, 0.f));
};

// A linear comparison sampler is the hardware's PCF tap: aimed a quarter of a
// texel short of the edge between the last near texel and the first far one,
// the far one weighs a quarter and is the only one the receiver is in front of.
// Greater would read three quarters, and a nearest sampler 0.
auto tLinearCompareFilters =
    test("ShadowSampling/aLinearComparisonAcrossAnEdgeGivesAFraction") = []
{
    if (!canTest())
        return;

    auto scene = Scene {};

    if (!scene.isValid())
        return;

    auto reader = LayerCompareReader {shadowLinear};
    reader.cascades = scene.layers;
    reader.layer = 1;
    reader.reference = 0.5f;
    reader.point = Array {0.4375f, 0.375f};
    reader.atPoint = 1.f;
    prepareReader(reader);

    const auto values = readThrough(scene, reader);

    for (auto value: values)
        check(near(value, 0.25f, 0.02f));
};

// The plain 2D form, over a TwoD target's plane.
auto tCompareAgainstAPlane =
    test("ShadowSampling/aDepth2DComparesThroughTheSameSampling") = []
{
    if (!canTest())
        return;

    auto scene = Scene {};

    if (!scene.isValid())
        return;

    auto reader = PlaneCompareReader {};
    reader.shadow = scene.plane;
    prepareReader(reader);

    reader.reference = 0.5f;
    check(holdsHalves(readThrough(scene, reader), 0.f, 1.f));

    reader.reference = 0.1f;
    check(holdsHalves(readThrough(scene, reader), 1.f, 1.f));
};
} // namespace ShadowSamplingTests

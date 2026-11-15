#include "Common.h"

#include <cstdint>
#include <functional>

// Statements in the vertex stage on the device: a loop that slides a quad by a
// uniform step count, a height map read at level 0 - through the texture's own
// sampling and through a sampler binding - that lifts each vertex by the texel
// under it, and a branch that pinches half a quad and picks its colour. Every
// one is observed in the pixels the draw leaves behind.
//
// Self-skips without a device; the sampler-binding case also without per-bind
// samplers (the D3D12 static-sampler fallback, see Lib/eacp/GPU/SAMPLERS.md).

using namespace nano;
using namespace eacp;
using namespace eacp::GPU;

namespace VertexStatementDeviceTests
{
namespace
{
struct QuadVertex
{
    float position[2];
};
} // namespace
} // namespace VertexStatementDeviceTests

EACP_SHADER_VALUE(VertexStatementDeviceTests::QuadVertex, Float2)

namespace VertexStatementDeviceTests
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

constexpr auto targetSize = 8;
constexpr auto halfSize = targetSize / 2;

bool hasDevice()
{
    return Device::shared().isValid();
}

std::uint32_t rgba(int red, int green, int blue)
{
    return 0xff000000u | ((std::uint32_t) blue << 16) | ((std::uint32_t) green << 8)
           | (std::uint32_t) red;
}

Texture makeTarget()
{
    auto descriptor = TextureDescriptor {};
    descriptor.width = targetSize;
    descriptor.height = targetSize;
    descriptor.format = TextureFormat::RGBA32Float;
    descriptor.renderTarget = true;
    return Device::shared().makeTexture(descriptor);
}

// Texel 0 lifts the vertices on the left edge, texel 1 those on the right.
Texture makeHeights(int left, int right)
{
    auto descriptor = TextureDescriptor {};
    descriptor.width = 2;
    descriptor.height = 1;

    const std::uint32_t texels[] = {rgba(left, 0, 0), rgba(right, 0, 0)};
    return Device::shared().makeTexture(descriptor, texels);
}

template <typename Shader>
void prepareShader(Shader& shader)
{
    shader.setVertices(fullQuad, 6);
    shader.prepare(1,
                   false,
                   PrimitiveTopology::Triangles,
                   BlendMode::None,
                   pixelFormatFor(TextureFormat::RGBA32Float));
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

struct Pixels
{
    const float* at(int x, int y) const
    {
        return values.data() + (y * targetSize + x) * 4;
    }

    bool isCovered(int x, int y) const
    {
        const auto* pixel = at(x, y);
        return pixel[0] + pixel[1] + pixel[2] > 0.5f;
    }

    Vector<float> values;
};

template <typename Shader>
Pixels drawAndRead(Shader& shader)
{
    auto target = makeTarget();
    auto pixels = Pixels {};
    pixels.values.resize(targetSize * targetSize * 4);

    auto draw = [&](Frame& frame)
    {
        {
            auto pass = frame.beginPass(target);
            pass.draw(shader);
        }

        frame.flush();
        target.read(pixels.values.data());
    };

    auto view = FrameRunner {};
    view.draw = draw;
    view.setBounds({0.f, 0.f, 4.f, 4.f});
    check(view.renderToImage(1.f).isValid());
    return pixels;
}

// Whether the pixels covered are exactly the columns [first, last) of row 0.
bool coversColumns(const Pixels& pixels, int first, int last)
{
    for (auto x = 0; x < targetSize; ++x)
        if (pixels.isCovered(x, 0) != (x >= first && x < last))
            return false;

    return true;
}

// Whether column x is covered exactly on the rows [first, last).
bool coversRows(const Pixels& pixels, int x, int first, int last)
{
    for (auto y = 0; y < targetSize; ++y)
        if (pixels.isCovered(x, y) != (y >= first && y < last))
            return false;

    return true;
}

// A half-width quad on the left, slid right by `stride` once per step: the
// loop count and the stride are uniforms its statements alone read.
struct SlidingQuad final : ShaderProgram
{
    SlidingQuad() { compile(); }

    void define() override
    {
        auto position = vertexInput(&QuadVertex::position);

        auto slide = [&]
        {
            auto shift = var(0.f);
            auto taken = var(0);

            auto advance = [&]
            {
                shift += stride;
                taken += 1;
            };

            loop(taken.get() < steps, advance);
            setPosition(float4(
                position.x() * 0.5f - 0.5f + shift.get(), position.y(), 0.f, 1.f));
        };

        vertexStage(slide);
        setFragment(float4(constant(1.f), 0.f, 0.f, 1.f));
    }

    Uniform<Int> steps;
    Uniform<Float> stride;

    EACP_SHADER(steps, stride)
};

// Each vertex reads the texel under its own column and rises by it: a lower
// half-height quad whose left edge, right edge or both are lifted to the top.
Float4 liftedPosition(const Float2& position, const Float& height)
{
    return float4(position.x(), position.y() * 0.5f - 0.5f + height, 0.f, 1.f);
}

Float2 columnOf(const Float2& position)
{
    return float2(position.x() * 0.5f + 0.5f, 0.5f);
}

constexpr auto nearestClamp =
    TextureSampling {TextureFilter::Nearest, TextureAddressMode::Clamp};

struct BakedHeightField final : ShaderProgram
{
    BakedHeightField()
    {
        heights.sampling = nearestClamp;
        compile();
    }

    void define() override
    {
        auto position = vertexInput(&QuadVertex::position);

        auto lift = [&]
        {
            auto height = var(sampleLevel(heights, columnOf(position), 0.f).x());
            setPosition(liftedPosition(position, height.get()));
        };

        vertexStage(lift);
        setFragment(float4(constant(0.f), 1.f, 0.f, 1.f));
    }

    Uniform<Texture2D> heights;

    EACP_SHADER(heights)
};

struct BoundHeightField final : ShaderProgram
{
    BoundHeightField() { compile(); }

    void define() override
    {
        auto position = vertexInput(&QuadVertex::position);

        auto lift = [&]
        {
            auto height =
                var(sampleLevel(heights, through, columnOf(position), 0.f).x());
            setPosition(liftedPosition(position, height.get()));
        };

        vertexStage(lift);
        setFragment(float4(constant(0.f), 1.f, 0.f, 1.f));
    }

    Uniform<Texture2D> heights;
    Uniform<SamplerBinding> through;

    EACP_SHADER(heights, through)
};

// A full quad, blue; folded, its left edge pinched onto the centre line and
// the colour switched to green, both inside one branch per vertex.
struct FoldingQuad final : ShaderProgram
{
    FoldingQuad() { compile(); }

    void define() override
    {
        auto position = vertexInput(&QuadVertex::position);
        auto shade = Float4 {};

        auto fold = [&]
        {
            auto x = var(position.x());
            auto colour = var(float4(constant(0.f), 0.f, 1.f, 1.f));

            auto pinch = [&] { x = 0.f; };
            auto foldLeft = [&]
            {
                ifThen(position.x() < 0.f, pinch);
                colour = float4(constant(0.f), 1.f, 0.f, 1.f);
            };

            ifThen(folded > 0.5f, foldLeft);
            setPosition(float4(x.get(), position.y(), 0.f, 1.f));
            shade = varying(colour.get());
        };

        vertexStage(fold);
        setFragment(shade);
    }

    Uniform<Float> folded;

    EACP_SHADER(folded)
};

template <typename HeightField>
void checkLifts(HeightField& field)
{
    const auto level = makeHeights(0, 0);
    const auto high = makeHeights(255, 255);
    const auto leftHigh = makeHeights(255, 0);

    field.heights = level;
    auto flat = drawAndRead(field);
    check(coversRows(flat, 0, halfSize, targetSize));
    check(coversRows(flat, targetSize - 1, halfSize, targetSize));

    field.heights = high;
    auto raised = drawAndRead(field);
    check(coversRows(raised, 0, 0, halfSize));
    check(coversRows(raised, targetSize - 1, 0, halfSize));

    field.heights = leftHigh;
    auto tilted = drawAndRead(field);
    check(coversRows(tilted, 0, 0, halfSize));
    check(coversRows(tilted, targetSize - 1, halfSize, targetSize));
    check(tilted.at(0, 0)[1] > 0.99f);
}
} // namespace

auto tLoop = test("VertexStatementDevice/aVertexLoopSlidesTheQuad") = []
{
    if (!hasDevice())
        return;

    auto quad = SlidingQuad {};
    prepareShader(quad);
    check(quad.vertexReadsUniforms());
    check(!quad.fragmentReadsUniforms());

    quad.steps = 0;
    quad.stride = 0.25f;
    check(coversColumns(drawAndRead(quad), 0, halfSize));

    quad.steps = 4;
    check(coversColumns(drawAndRead(quad), halfSize, targetSize));

    quad.steps = 2;
    quad.stride = 0.5f;
    auto moved = drawAndRead(quad);
    check(coversColumns(moved, halfSize, targetSize));
    check(moved.at(halfSize, 0)[0] > 0.99f);
};

auto tBakedHeights =
    test("VertexStatementDevice/aHeightMapReadAtLevelZeroLiftsEachVertex") = []
{
    if (!hasDevice())
        return;

    auto field = BakedHeightField {};
    prepareShader(field);
    check(field.vertexReadsUniforms() == false);
    checkLifts(field);
};

auto tBoundHeights =
    test("VertexStatementDevice/aSamplerBindingReachesTheVertexStage") = []
{
    if (!hasDevice() || !Device::shared().supportsPerBindSamplers())
        return;

    auto descriptor = SamplerDescriptor {};
    descriptor.minFilter = SamplerFilter::Nearest;
    descriptor.magFilter = SamplerFilter::Nearest;
    descriptor.addressU = SamplerAddressMode::ClampToEdge;
    descriptor.addressV = SamplerAddressMode::ClampToEdge;

    auto field = BoundHeightField {};
    field.through = Device::shared().makeSampler(descriptor);
    prepareShader(field);
    checkLifts(field);
};

auto tBranch = test("VertexStatementDevice/aVertexBranchShowsInThePixels") = []
{
    if (!hasDevice())
        return;

    auto quad = FoldingQuad {};
    prepareShader(quad);

    quad.folded = 0.f;
    auto whole = drawAndRead(quad);
    check(coversColumns(whole, 0, targetSize));
    check(whole.at(0, 0)[2] > 0.99f && whole.at(0, 0)[1] < 0.01f);

    quad.folded = 1.f;
    auto folded = drawAndRead(quad);
    check(coversColumns(folded, halfSize, targetSize));
    check(folded.at(targetSize - 1, 0)[1] > 0.99f);
    check(folded.at(targetSize - 1, 0)[2] < 0.01f);
};

auto tExplicitForms = test(
    "VertexStatementDevice/everyExplicitLevelFormCompilesInAVertexFunction") = []
{
    if (!hasDevice())
        return;

    auto builder = ShaderBuilder {};
    auto position = builder.vertexInput<Float2>();
    auto colour = builder.texture();
    auto layers = builder.textureArray();
    auto volume = builder.texture3D();
    auto shadow = builder.depthTexture({TextureFilter::Linear,
                                        TextureAddressMode::Clamp,
                                        TextureCompare::LessEqual});
    auto compare = builder.comparisonSampler();
    auto plain = builder.depthTexture();
    auto smooth = builder.sampler();

    auto read = [&]
    {
        auto uv = position * 0.5f + 0.5f;
        auto sum = builder.var(sampleGrad(colour, uv, uv * 0.f, uv * 0.f));
        sum += sampleLevel(layers, uv, 1, 0.f);
        sum += sampleLevel(volume, float3(uv, 0.5f), 0.f);
        sum += sampleLevel(colour, smooth, uv, 0.f);
        sum += sample(colour, uv, 0.f);
        sum += gather(colour, uv, 2);
        sum += load(colour, int2(builder.integer(0), 0), 0);
        sum += sampleCompareLevel(shadow, uv, 0.5f);
        sum += sampleCompareLevel(plain, compare, uv, 0.5f);
        sum += gatherCompare(plain, compare, uv, 0.5f);
        sum += toFloat(dimensions(colour).x());
        builder.position(float4(position, sum.get().x(), 1.f));
    };

    builder.vertexStage(read);
    builder.fragment(float4(builder.constant(1.f), 1.f, 1.f, 1.f));

    auto library = Device::shared().makeShaderLibrary(builder.build().source);
    check(library.isValid());
};
} // namespace VertexStatementDeviceTests

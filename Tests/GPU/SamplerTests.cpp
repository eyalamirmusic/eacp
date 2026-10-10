#include "Common.h"

#include <algorithm>
#include <cmath>
#include <unordered_set>

// Device::makeSampler and the per-bind sampler overloads: a sampler chosen at
// the bind rather than declared by the shader. The shaders here are written by
// hand, the way an imported one is (bgfx's MSL takes `sampler [[sampler(n)]]`
// arguments), and every pixel a draw produces is checked against what the
// descriptor says the sampler must return.
//
// The drawing cases self-skip without a device, and where the device reports
// !supportsPerBindSamplers() - the D3D12 static-sampler fallback, see
// Lib/eacp/GPU/SAMPLERS.md.

using namespace nano;
using namespace eacp;
using namespace eacp::GPU;

namespace
{
constexpr auto viewWidth = 16;
constexpr auto viewHeight = 4;

constexpr std::uint32_t red = 0xff0000ff;
constexpr std::uint32_t green = 0xff00ff00;
constexpr std::uint32_t blue = 0xffff0000;
constexpr std::uint32_t white = 0xffffffff;

struct QuadVertex
{
    float position[2];
    float uv[2];
};

// Covers x in [left, right] of the target, u running u0..u1 across it, and v
// held on the centre of the texture's top row so only u is ever addressed.
Array<QuadVertex, 6> quadOver(float left, float right, float u0, float u1)
{
    constexpr auto v = 0.25f;

    return {{
        {{left, -1.f}, {u0, v}},
        {{right, -1.f}, {u1, v}},
        {{left, 1.f}, {u0, v}},
        {{right, -1.f}, {u1, v}},
        {{right, 1.f}, {u1, v}},
        {{left, 1.f}, {u0, v}},
    }};
}

// The whole target, sampling the whole texture.
constexpr QuadVertex fullQuad[] = {
    {{-1.f, -1.f}, {0.f, 1.f}},
    {{1.f, -1.f}, {1.f, 1.f}},
    {{-1.f, 1.f}, {0.f, 0.f}},
    {{1.f, -1.f}, {1.f, 1.f}},
    {{1.f, 1.f}, {1.f, 0.f}},
    {{-1.f, 1.f}, {0.f, 0.f}},
};

const char* mslSampling = R"(
#include <metal_stdlib>
using namespace metal;

struct VertexIn
{
    float2 position [[attribute(0)]];
    float2 uv [[attribute(1)]];
};

struct VertexOut
{
    float4 position [[position]];
    float2 uv;
};

vertex VertexOut vertexMain(VertexIn in [[stage_in]])
{
    VertexOut out;
    out.position = float4(in.position, 0.0, 1.0);
    out.uv = in.uv;
    return out;
}

fragment float4 fragmentMain(VertexOut in [[stage_in]],
                             texture2d<float> image [[texture(0)]],
                             sampler imageSampler [[sampler(0)]])
{
    return image.sample(imageSampler, in.uv);
}
)";

const char* hlslSampling = R"(
Texture2D image : register(t0);
SamplerState imageSampler : register(s8);

struct VertexIn
{
    float2 position : TEXCOORD0;
    float2 uv : TEXCOORD1;
};

struct VertexOut
{
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
};

VertexOut vertexMain(VertexIn input)
{
    VertexOut o;
    o.position = float4(input.position, 0.0, 1.0);
    o.uv = input.uv;
    return o;
}

float4 fragmentMain(VertexOut input) : SV_Target
{
    return image.Sample(imageSampler, input.uv);
}
)";

const char* glslSampling = R"(#version 450

#ifdef EACP_VERTEX
layout(location = 0) in vec2 attr0;
layout(location = 1) in vec2 attr1;
layout(location = 0) out vec2 uv;

void main()
{
    gl_Position = vec4(attr0, 0.0, 1.0);
    uv = attr1;
}
#endif

#ifdef EACP_FRAGMENT
layout(location = 0) in vec2 uv;
layout(set = 0, binding = 8) uniform sampler2D image;
layout(location = 0) out vec4 fragColor;

void main()
{
    fragColor = texture(image, uv);
}
#endif
)";

const char* mslCompare = R"(
#include <metal_stdlib>
using namespace metal;

struct VertexIn
{
    float2 position [[attribute(0)]];
    float2 uv [[attribute(1)]];
};

struct VertexOut
{
    float4 position [[position]];
    float2 uv;
};

vertex VertexOut vertexMain(VertexIn in [[stage_in]])
{
    VertexOut out;
    out.position = float4(in.position, 0.0, 1.0);
    out.uv = in.uv;
    return out;
}

fragment float4 fragmentMain(VertexOut in [[stage_in]],
                             depth2d<float> depthImage [[texture(0)]],
                             sampler depthSampler [[sampler(0)]])
{
    float passed = depthImage.sample_compare(depthSampler, in.uv, 0.5);
    return float4(passed, 0.0, 0.0, 1.0);
}
)";

const char* hlslCompare = R"(
Texture2D<float> depthImage : register(t0);
SamplerComparisonState depthSampler : register(s8);

struct VertexIn
{
    float2 position : TEXCOORD0;
    float2 uv : TEXCOORD1;
};

struct VertexOut
{
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
};

VertexOut vertexMain(VertexIn input)
{
    VertexOut o;
    o.position = float4(input.position, 0.0, 1.0);
    o.uv = input.uv;
    return o;
}

float4 fragmentMain(VertexOut input) : SV_Target
{
    float passed = depthImage.SampleCmpLevelZero(depthSampler, input.uv, 0.5);
    return float4(passed, 0.0, 0.0, 1.0);
}
)";

const char* glslCompare = R"(#version 450

#ifdef EACP_VERTEX
layout(location = 0) in vec2 attr0;
layout(location = 1) in vec2 attr1;
layout(location = 0) out vec2 uv;

void main()
{
    gl_Position = vec4(attr0, 0.0, 1.0);
    uv = attr1;
}
#endif

#ifdef EACP_FRAGMENT
layout(location = 0) in vec2 uv;
layout(set = 0, binding = 8) uniform sampler2DShadow depthImage;
layout(location = 0) out vec4 fragColor;

void main()
{
    float passed = texture(depthImage, vec3(uv, 0.5));
    fragColor = vec4(passed, 0.0, 0.0, 1.0);
}
#endif
)";

RenderPipeline makeQuadPipeline(const ShaderLibrary& library, int sampleCount)
{
    auto descriptor = RenderPipelineDescriptor {};
    descriptor.library = &library;
    descriptor.sampleCount = sampleCount;
    descriptor.vertexLayout.attribute(VertexFormat::Float2, 0);
    descriptor.vertexLayout.attribute(VertexFormat::Float2, 8);
    descriptor.vertexLayout.stride = sizeof(QuadVertex);

    return Device::shared().makeRenderPipeline(descriptor);
}

ShaderLibrary makeLibrary(ShaderSource source)
{
    return Device::shared().makeShaderLibrary(
        source.withVertex("vertexMain").withFragment("fragmentMain"));
}

// Red, green on the top row; blue, white under them.
Texture makeTwoByTwoTexture()
{
    static std::uint32_t pixels[] = {red, green, blue, white};

    auto descriptor = TextureDescriptor {};
    descriptor.width = 2;
    descriptor.height = 2;
    descriptor.format = TextureFormat::RGBA8Unorm;

    return Device::shared().makeTexture(descriptor, pixels);
}

// Each draw is one quad through one pipeline, so whatever differs between two
// draws is the sampler bound for it.
struct SamplerView final : GPUView
{
    explicit SamplerView(Texture& textureToSample)
        : texture(textureToSample)
        , library(makeLibrary(
              nativeShaderSource(mslSampling, hlslSampling, glslSampling)))
        , pipeline(makePipeline())
    {
    }

    RenderPipeline makePipeline()
    {
        setSampleCount(1);
        return makeQuadPipeline(library, sampleCount());
    }

    void addDraw(const Array<QuadVertex, 6>& quad, const Sampler& sampler)
    {
        draws.add({Device::shared().makeBuffer(quad.data(), sizeof(quad)), sampler});
    }

    void render(Frame& frame) override
    {
        auto pass = frame.beginPass({{0.f, 0.f, 0.f, 1.f}});
        pass.setPipeline(pipeline);

        for (auto& draw: draws)
        {
            pass.setVertexBuffer(draw.vertices);
            pass.setFragmentTexture(texture, 0, draw.sampler);
            pass.draw(6);
        }
    }

    Graphics::Image readBack()
    {
        setBounds({0.f, 0.f, (float) viewWidth, (float) viewHeight});
        return renderToImage(1.f);
    }

    struct Draw
    {
        Buffer vertices;
        Sampler sampler;
    };

    Texture& texture;
    ShaderLibrary library;
    RenderPipeline pipeline;
    Vector<Draw> draws;
};

bool canDrawPerBind()
{
    return Device::shared().isValid() && Device::shared().supportsPerBindSamplers();
}

SamplerDescriptor nearestWith(SamplerAddressMode mode)
{
    auto descriptor = SamplerDescriptor {};
    descriptor.addressU = mode;
    descriptor.addressV = mode;
    return descriptor;
}

float centreU(int x, float u0, float u1, int pixels)
{
    return u0 + (u1 - u0) * ((float) x + 0.5f) / (float) pixels;
}

float repeated(float u)
{
    return u - std::floor(u);
}

float mirrored(float u)
{
    const auto period = u - 2.f * std::floor(u * 0.5f);
    return period < 1.f ? period : 2.f - period;
}

std::uint32_t topRowTexel(float u)
{
    return std::clamp(u, 0.f, 1.f) < 0.5f ? red : green;
}

// What a nearest sampler with this address mode returns at u on the top row.
std::uint32_t expectedNearest(SamplerAddressMode mode, float u)
{
    switch (mode)
    {
        case SamplerAddressMode::ClampToEdge:
            return topRowTexel(u);
        case SamplerAddressMode::Repeat:
            return topRowTexel(repeated(u));
        case SamplerAddressMode::MirrorRepeat:
            return topRowTexel(mirrored(u));
        case SamplerAddressMode::ClampToBorder:
            return u < 0.f || u > 1.f ? white : topRowTexel(u);
    }

    return 0;
}

bool matches(const Graphics::Color& c, std::uint32_t rgba)
{
    const auto channel = [rgba](int shift)
    { return (float) ((rgba >> shift) & 0xff) / 255.f; };

    constexpr auto tolerance = 0.05f;

    return std::abs(c.r - channel(0)) < tolerance
           && std::abs(c.g - channel(8)) < tolerance
           && std::abs(c.b - channel(16)) < tolerance;
}

// Every pixel of columns [first, first + pixels) against the nearest sampler's
// answer for u0..u1 across them.
int countMismatches(const Graphics::Image& image,
                    int first,
                    int pixels,
                    SamplerAddressMode mode,
                    float u0,
                    float u1)
{
    auto mismatches = 0;

    for (auto y = 0; y < image.height(); ++y)
        for (auto x = 0; x < pixels; ++x)
        {
            const auto expected = expectedNearest(mode, centreU(x, u0, u1, pixels));

            if (!matches(image.at(first + x, y), expected))
                ++mismatches;
        }

    return mismatches;
}

// Draws the full width at u -1..3 - four widths of the texture, so every
// address mode has room to differ - and counts the pixels that disagree.
int mismatchesAcrossFourTiles(SamplerDescriptor descriptor)
{
    auto texture = makeTwoByTwoTexture();

    if (!texture.isValid())
        return -1;

    auto view = SamplerView {texture};

    if (!view.pipeline.isValid())
        return -1;

    view.addDraw(quadOver(-1.f, 1.f, -1.f, 3.f),
                 Device::shared().makeSampler(descriptor));

    auto image = view.readBack();

    if (!image.isValid() || image.width() != viewWidth)
        return -1;

    return countMismatches(image, 0, viewWidth, descriptor.addressU, -1.f, 3.f);
}
} // namespace

auto tHashFollowsEquality = test("Sampler/descriptorsHashAndCompareByValue") = []
{
    auto a = nearestWith(SamplerAddressMode::Repeat);
    auto b = nearestWith(SamplerAddressMode::Repeat);
    auto c = nearestWith(SamplerAddressMode::MirrorRepeat);

    check(a == b);
    check(hashOf(a) == hashOf(b));
    check(!(a == c));

    auto set = std::unordered_set<SamplerDescriptor> {a, b, c};
    check(set.size() == 2);

    auto compared = a;
    compared.compare = CompareFunction::Less;
    check(!(compared == a));
};

auto tCacheReturnsOneObject =
    test("Sampler/equalDescriptorsShareOneNativeSampler") = []
{
    if (!Device::shared().isValid())
        return;

    auto descriptor = nearestWith(SamplerAddressMode::MirrorRepeat);
    descriptor.borderColor = SamplerBorderColor::OpaqueWhite;

    auto first = Device::shared().makeSampler(descriptor);
    auto second = Device::shared().makeSampler(descriptor);

    check(first.isValid());
    check(first.nativeSampler() == second.nativeSampler());
    check(first == second);
    check(first.descriptor() == descriptor);
};

auto tCacheSeparatesDescriptors =
    test("Sampler/differentDescriptorsGetDifferentSamplers") = []
{
    if (!Device::shared().isValid())
        return;

    auto repeat =
        Device::shared().makeSampler(nearestWith(SamplerAddressMode::Repeat));
    auto mirror =
        Device::shared().makeSampler(nearestWith(SamplerAddressMode::MirrorRepeat));

    auto linear = nearestWith(SamplerAddressMode::Repeat);
    linear.magFilter = SamplerFilter::Linear;
    auto smooth = Device::shared().makeSampler(linear);

    auto shadow = nearestWith(SamplerAddressMode::ClampToEdge);
    shadow.compare = CompareFunction::LessEqual;
    auto compare = Device::shared().makeSampler(shadow);

    check(repeat.isValid() && mirror.isValid() && smooth.isValid()
          && compare.isValid());

    auto natives = std::unordered_set<void*> {repeat.nativeSampler(),
                                              mirror.nativeSampler(),
                                              smooth.nativeSampler(),
                                              compare.nativeSampler()};
    check(natives.size() == 4);
};

// The TextureSampling overloads bind the sampler of the matching descriptor,
// so the four configurations are entries of the same cache.
auto tSamplingIsADescriptor =
    test("Sampler/textureSamplingIsOneOfTheCachedSamplers") = []
{
    if (!Device::shared().isValid())
        return;

    for (auto filter: {TextureFilter::Nearest, TextureFilter::Linear})
        for (auto mode: {TextureAddressMode::Clamp, TextureAddressMode::Repeat})
        {
            const auto sampling = TextureSampling {filter, mode};
            auto sampler =
                Device::shared().makeSampler(SamplerDescriptor::from(sampling));

            check(sampler.isValid());

            if (Device::shared().nativeSampler(sampling) != nullptr)
                check(sampler.nativeSampler()
                      == Device::shared().nativeSampler(sampling));
        }
};

auto tClampPerBind = test("Sampler/clampHoldsTheEdgeTexels") = []
{
    if (!canDrawPerBind())
        return;

    check(mismatchesAcrossFourTiles(nearestWith(SamplerAddressMode::ClampToEdge))
          == 0);
};

auto tRepeatPerBind = test("Sampler/repeatTilesTheTexture") = []
{
    if (!canDrawPerBind())
        return;

    check(mismatchesAcrossFourTiles(nearestWith(SamplerAddressMode::Repeat)) == 0);
};

auto tMirrorPerBind = test("Sampler/mirrorReflectsEveryOtherTile") = []
{
    if (!canDrawPerBind())
        return;

    check(mismatchesAcrossFourTiles(nearestWith(SamplerAddressMode::MirrorRepeat))
          == 0);
};

auto tBorderPerBind = test("Sampler/clampToBorderReadsTheBorderColour") = []
{
    if (!canDrawPerBind())
        return;

    auto descriptor = nearestWith(SamplerAddressMode::ClampToBorder);
    descriptor.borderColor = SamplerBorderColor::OpaqueWhite;

    check(mismatchesAcrossFourTiles(descriptor) == 0);
};

// One pipeline, one texture, two draws: the left half repeats and the right
// half clamps. A backend that took the sampler from the shader, or from the
// first bind of the pass, gets one of the halves wrong.
auto tTwoSamplersOnePass = test("Sampler/oneProgramSamplesTwoWaysInOnePass") = []
{
    if (!canDrawPerBind())
        return;

    auto texture = makeTwoByTwoTexture();

    if (!texture.isValid())
        return;

    auto view = SamplerView {texture};

    if (!view.pipeline.isValid())
        return;

    view.addDraw(
        quadOver(-1.f, 0.f, -1.f, 3.f),
        Device::shared().makeSampler(nearestWith(SamplerAddressMode::Repeat)));
    view.addDraw(
        quadOver(0.f, 1.f, -1.f, 3.f),
        Device::shared().makeSampler(nearestWith(SamplerAddressMode::ClampToEdge)));

    auto image = view.readBack();

    check(image.isValid());

    const auto half = viewWidth / 2;

    check(countMismatches(image, 0, half, SamplerAddressMode::Repeat, -1.f, 3.f)
          == 0);
    check(countMismatches(
              image, half, half, SamplerAddressMode::ClampToEdge, -1.f, 3.f)
          == 0);
};

auto tNearestPerBind = test("Sampler/nearestKeepsTexelsDistinct") = []
{
    if (!canDrawPerBind())
        return;

    auto texture = makeTwoByTwoTexture();

    if (!texture.isValid())
        return;

    auto view = SamplerView {texture};

    if (!view.pipeline.isValid())
        return;

    view.addDraw(quadOver(-1.f, 1.f, 0.f, 1.f),
                 Device::shared().makeSampler(SamplerDescriptor {}));

    auto image = view.readBack();

    check(image.isValid());
    check(countMismatches(
              image, 0, viewWidth, SamplerAddressMode::ClampToEdge, 0.f, 1.f)
          == 0);
};

// Linear weights the two texel centres by distance: at u the green weight is
// u * 2 - 0.5, held to 0..1 by the clamp at either edge.
auto tLinearPerBind = test("Sampler/linearWeighsTheTexelCentres") = []
{
    if (!canDrawPerBind())
        return;

    auto texture = makeTwoByTwoTexture();

    if (!texture.isValid())
        return;

    auto view = SamplerView {texture};

    if (!view.pipeline.isValid())
        return;

    auto descriptor = SamplerDescriptor {};
    descriptor.minFilter = SamplerFilter::Linear;
    descriptor.magFilter = SamplerFilter::Linear;

    view.addDraw(quadOver(-1.f, 1.f, 0.f, 1.f),
                 Device::shared().makeSampler(descriptor));

    auto image = view.readBack();

    check(image.isValid());

    auto mismatches = 0;
    auto blended = 0;

    for (auto y = 0; y < image.height(); ++y)
        for (auto x = 0; x < viewWidth; ++x)
        {
            const auto u = centreU(x, 0.f, 1.f, viewWidth);
            const auto weight = std::clamp(u * 2.f - 0.5f, 0.f, 1.f);
            const auto colour = image.at(x, y);

            if (std::abs(colour.g - weight) > 0.03f
                || std::abs(colour.r - (1.f - weight)) > 0.03f)
                ++mismatches;

            if (weight > 0.1f && weight < 0.9f)
                ++blended;
        }

    check(mismatches == 0);
    check(blended > 0);
};

namespace
{
constexpr auto depthTargetSize = 4;
constexpr auto writtenDepth = 0.25f;

// The left half only, so the right half of the depth buffer keeps its clear.
constexpr QuadVertex leftHalf[] = {
    {{-1.f, -1.f}, {0.f, 1.f}},
    {{0.f, -1.f}, {0.5f, 1.f}},
    {{-1.f, 1.f}, {0.f, 0.f}},
    {{0.f, -1.f}, {0.5f, 1.f}},
    {{0.f, 1.f}, {0.5f, 0.f}},
    {{-1.f, 1.f}, {0.f, 0.f}},
};
} // namespace

EACP_SHADER_VALUE(QuadVertex, Float2)

namespace
{
struct DepthWriter final : ShaderProgram
{
    DepthWriter() { compile(); }

    void define() override
    {
        auto position = vertexInput(&QuadVertex::position);

        setPosition(float4(position, writtenDepth, 1.f));
        setFragment(float4(color, 1.f));
    }

    Uniform<Float3> color;

    EACP_SHADER(color)
};

TextureDescriptor describeDepthScene()
{
    auto descriptor = TextureDescriptor {};
    descriptor.width = depthTargetSize;
    descriptor.height = depthTargetSize;
    descriptor.format = TextureFormat::RGBA8Unorm;
    descriptor.renderTarget = true;
    descriptor.sampleableDepth = true;
    return descriptor;
}

// Writes depth 0.25 over the left half of a target, then compares the reference
// 0.5 against that depth across the whole view.
struct CompareView final : GPUView
{
    explicit CompareView(CompareFunction function)
        : scene(Device::shared().makeTexture(describeDepthScene()))
        , library(
              makeLibrary(nativeShaderSource(mslCompare, hlslCompare, glslCompare)))
        , pipeline(makePipeline())
        , vertices(Device::shared().makeBuffer(fullQuad))
        , sampler(Device::shared().makeSampler(describeCompare(function)))
    {
        writer.setVertices(leftHalf, 6);
        writer.color = Array {1.f, 1.f, 1.f};
        writer.prepare(1,
                       true,
                       PrimitiveTopology::Triangles,
                       BlendMode::None,
                       pixelFormatFor(TextureFormat::RGBA8Unorm));
    }

    static SamplerDescriptor describeCompare(CompareFunction function)
    {
        auto descriptor = SamplerDescriptor {};
        descriptor.compare = function;
        return descriptor;
    }

    RenderPipeline makePipeline()
    {
        setSampleCount(1);
        return makeQuadPipeline(library, sampleCount());
    }

    void render(Frame& frame) override
    {
        {
            auto descriptor = RenderPassDescriptor {};
            descriptor.clearColor = Graphics::Color::black();
            descriptor.depthAction = DepthAction::Keep;

            auto into = frame.beginPass(scene, descriptor);
            into.draw(writer);
        }

        auto pass = frame.beginPass({Graphics::Color::black()});
        pass.setPipeline(pipeline);
        pass.setVertexBuffer(vertices);
        pass.setFragmentDepthTexture(scene, 0, sampler);
        pass.draw(6);
    }

    Graphics::Image readBack()
    {
        setBounds({0.f, 0.f, (float) viewWidth, (float) viewHeight});
        return renderToImage(1.f);
    }

    bool isReady() const
    {
        return scene.hasSampleableDepth() && pipeline.isValid() && sampler.isValid();
    }

    Texture scene;
    ShaderLibrary library;
    RenderPipeline pipeline;
    Buffer vertices;
    Sampler sampler;
    DepthWriter writer;
};

bool passed(const Graphics::Color& c)
{
    return c.r > 0.9f;
}

bool failed(const Graphics::Color& c)
{
    return c.r < 0.1f;
}

// How many pixels of each half passed the comparison, as {left, right}.
std::pair<int, int> passedPerHalf(const Graphics::Image& image)
{
    auto left = 0;
    auto right = 0;

    for (auto y = 0; y < image.height(); ++y)
        for (auto x = 0; x < image.width(); ++x)
        {
            const auto colour = image.at(x, y);

            if (!passed(colour) && !failed(colour))
                return {-1, -1};

            if (passed(colour))
                ++(x < image.width() / 2 ? left : right);
        }

    return {left, right};
}
} // namespace

// Less: the reference 0.5 is less than the cleared 1.0 on the right and not
// less than the 0.25 written on the left.
auto tCompareLess = test("Sampler/compareLessPassesOnlyBehindTheReference") = []
{
    if (!canDrawPerBind())
        return;

    auto view = CompareView {CompareFunction::Less};

    if (!view.isReady())
        return;

    auto image = view.readBack();

    check(image.isValid());

    const auto [left, right] = passedPerHalf(image);
    const auto half = viewWidth / 2 * viewHeight;

    check(left == 0);
    check(right == half);
};

// The same scene through Greater gives the opposite halves, which is what says
// the function came from the sampler rather than a fixed default.
auto tCompareGreater = test("Sampler/compareGreaterPassesOnlyInFront") = []
{
    if (!canDrawPerBind())
        return;

    auto view = CompareView {CompareFunction::Greater};

    if (!view.isReady())
        return;

    auto image = view.readBack();

    check(image.isValid());

    const auto [left, right] = passedPerHalf(image);
    const auto half = viewWidth / 2 * viewHeight;

    check(left == half);
    check(right == 0);
};

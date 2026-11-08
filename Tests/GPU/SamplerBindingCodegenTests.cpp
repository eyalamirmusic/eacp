#include "CodegenCommon.h"

#include <eacp/Core/Utils/Format.h>
#include <eacp/GPU/Codegen/ComputeKernel.h>
#include <eacp/GPU/Codegen/Samplers.h>

#include <initializer_list>
#include <string>

// Samplers declared as bindings of their own (Samplers.h), as text in all three
// dialects, each GLSL source compiled wherever eacp-spirv is built: where each
// sampler is declared, every sampling function on every kind it takes reading
// through one, and the baked TextureSampling forms left exactly as they were.
// What the bound sampler does on the device is SamplerBindingDeviceTests'.

using namespace nano;
using namespace eacp;
using namespace eacp::GPU;

namespace SamplerBindingCodegenTests
{
namespace
{
struct Sources
{
    explicit Sources(const ShaderGraph& graph)
        : metal(emitMetal(graph))
        , hlsl(emitHlsl(graph))
        , glsl(emitGlsl(graph))
    {
    }

    std::string metal;
    std::string hlsl;
    std::string glsl;
};

// u0 a coordinate, u1 a direction, u2 a layer, u3 a float level or bias, u4
// an integer level, u5 a depth reference, u6 and u7 two gradients of a 2D
// coordinate, u8 and u9 of a 3D one.
struct Arguments
{
    explicit Arguments(ShaderBuilder& builder)
        : uv(builder.uniform<Float2>())
        , direction(builder.uniform<Float3>())
        , layer(builder.uniform<Int>())
        , level(builder.uniform<Float>())
        , index(builder.uniform<Int>())
        , reference(builder.uniform<Float>())
        , ddx(builder.uniform<Float2>())
        , ddy(builder.uniform<Float2>())
        , ddx3(builder.uniform<Float3>())
        , ddy3(builder.uniform<Float3>())
    {
    }

    Float2 uv;
    Float3 direction;
    Int layer;
    Float level;
    Int index;
    Float reference;
    Float2 ddx;
    Float2 ddy;
    Float3 ddx3;
    Float3 ddy3;
};

struct Fragment
{
    Fragment()
    {
        auto position = builder.vertexInput<Float2>();
        builder.position(float4(position, 0.f, 1.f));
    }

    ShaderBuilder builder;
    Arguments in {builder};
};

// Five colour kinds at slots 0 to 4 and a sampler at 5.
struct ColourKinds
{
    explicit ColourKinds(ShaderBuilder& builder)
        : image(builder.texture())
        , layers(builder.textureArray())
        , volume(builder.texture3D())
        , cube(builder.cubeTexture())
        , cubes(builder.cubeTextureArray())
        , smooth(builder.sampler())
    {
    }

    Texture2D image;
    Texture2DArray layers;
    Texture3D volume;
    TextureCube cube;
    TextureCubeArray cubes;
    SamplerBinding smooth;
};

// Four depth kinds at slots 0 to 3, a plain sampler at 4 and a comparison one
// at 5.
struct DepthKinds
{
    explicit DepthKinds(ShaderBuilder& builder)
        : plane(builder.depthTexture())
        , planes(builder.depthTextureArray())
        , cube(builder.depthCubeTexture())
        , cubes(builder.depthCubeTextureArray())
        , plain(builder.sampler())
        , shadow(builder.comparisonSampler())
    {
    }

    TextureDepth2D plane;
    TextureDepth2DArray planes;
    TextureDepthCube cube;
    TextureDepthCubeArray cubes;
    SamplerBinding plain;
    ComparisonSamplerBinding shadow;
};

Float4 widen(const Float& a, const Float& b, const Float& c, const Float& d)
{
    return float4(a, b, c, d);
}

void checkAll(const std::string& source,
              std::initializer_list<const char*> expected,
              const std::source_location& location = std::source_location::current())
{
    for (const auto* text: expected)
        check(contains(source, text), text, location);
}

// Which function takes which sampler is the type's to say, as in WGSL.
template <typename Texture, typename Sampler>
concept SamplesThrough =
    requires(const Texture& texture, const Sampler& sampler, const Float2& uv) {
        sample(texture, sampler, uv);
    };

template <typename Texture, typename Sampler>
concept ComparesThrough = requires(const Texture& texture,
                                   const Sampler& sampler,
                                   const Float2& uv,
                                   const Float& reference) {
    sampleCompare(texture, sampler, uv, reference);
};

template <typename Texture>
concept GathersThrough =
    requires(const Texture& texture,
             const SamplerBinding& sampler,
             const Float3& coordinates) { gather(texture, sampler, coordinates); };

static_assert(SamplesThrough<Texture2D, SamplerBinding>);
static_assert(SamplesThrough<Uniform<Texture2D>, Uniform<SamplerBinding>>);
static_assert(!SamplesThrough<Texture2D, ComparisonSamplerBinding>);
static_assert(SamplesThrough<TextureDepth2D, SamplerBinding>);
static_assert(ComparesThrough<TextureDepth2D, ComparisonSamplerBinding>);
static_assert(!ComparesThrough<TextureDepth2D, SamplerBinding>);
static_assert(!ComparesThrough<Texture2D, ComparisonSamplerBinding>);
static_assert(GathersThrough<TextureCube>);
static_assert(!GathersThrough<Texture3D>);
} // namespace

auto tDeclarations =
    test("SamplerBindingCodegen/eachSamplerIsDeclaredAtItsOwnSlot") = []
{
    auto shader = Fragment {};
    auto& builder = shader.builder;
    auto image = builder.texture();
    auto smooth = builder.sampler();
    auto shadow = builder.comparisonSampler();
    auto depth = builder.depthTexture();
    const auto& in = shader.in;

    builder.fragment(sample(image, smooth, in.uv)
                     * sampleCompare(depth, shadow, in.uv, in.reference));

    auto sources = Sources {builder.graph()};

    checkAll(sources.metal,
             {"texture2d<float> texture0 [[texture(0)]]",
              "sampler sampler0 [[sampler(0)]]",
              "sampler sampler1 [[sampler(1)]]",
              "sampler sampler2 [[sampler(2)]]",
              "depth2d<float> texture3 [[texture(3)]]",
              "sampler sampler3 [[sampler(3)]]",
              "texture0.sample(sampler1, uniforms.u0)",
              "texture3.sample_compare(sampler2, uniforms.u0, uniforms.u5)"});
    check(!contains(sources.metal, "texture1 "));
    check(!contains(sources.metal, "texture2 "));

    checkAll(sources.hlsl,
             {"Texture2D texture0 : register(t0);",
              "SamplerState sampler1 : register(s9);",
              "SamplerComparisonState sampler2 : register(s10);",
              "Texture2D<float> texture3 : register(t3);",
              "texture0.Sample(sampler1, uniforms.u0)",
              "texture3.SampleCmpLevelZero(sampler2, uniforms.u0, uniforms.u5)"});
    check(!contains(sources.hlsl, "texture1"));

    checkAll(sources.glsl,
             {"layout(set = 0, binding = 8) uniform sampler2D texture0;",
              "layout(set = 0, binding = 8) uniform texture2D textureImage0;",
              "layout(set = 0, binding = 9) uniform sampler sampler1;",
              "layout(set = 0, binding = 10) uniform samplerShadow sampler2;",
              "layout(set = 0, binding = 11) uniform sampler2D texture3;",
              "layout(set = 0, binding = 11) uniform texture2D textureImage3;",
              "texture(sampler2D(textureImage0, sampler1), uniforms.u0)",
              "texture(sampler2DShadow(textureImage3, sampler2), "
              "vec3(uniforms.u0, uniforms.u5))"});

    expectGlslCompiles(builder.graph());
};

// A texture only ever sampled its own way declares no separate image, and a
// sampler slot no static sampler: HLSL's samplerConfig set is the textures'.
auto tNothingUnasked =
    test("SamplerBindingCodegen/aBakedTextureGetsNoSeparateImage") = []
{
    auto shader = Fragment {};
    auto& builder = shader.builder;
    auto baked = builder.texture(
        {TextureFilter::Linear, TextureAddressMode::Repeat, TextureCompare::None});
    auto bound = builder.texture();
    auto smooth = builder.sampler();
    const auto& in = shader.in;

    builder.fragment(sample(baked, in.uv) + sample(bound, smooth, in.uv));

    auto sources = Sources {builder.graph()};

    check(!contains(sources.glsl, "textureImage0"));
    check(contains(sources.glsl, "uniform texture2D textureImage1;"));
    check(contains(sources.hlsl, "SamplerState samplerConfig3 : register(s3);"));
    check(contains(sources.hlsl, "SamplerState samplerConfig0 : register(s0);"));
    check(occurrences(sources.hlsl, "samplerConfig") == 3);
    check(contains(sources.hlsl, "texture0.Sample(samplerConfig3, uniforms.u0)"));

    expectGlslCompiles(builder.graph());
};

auto tSampleColour =
    test("SamplerBindingCodegen/sampleThroughASamplerOnEveryColourKind") = []
{
    auto shader = Fragment {};
    auto kinds = ColourKinds {shader.builder};
    const auto& in = shader.in;
    const auto& smooth = kinds.smooth;

    shader.builder.fragment(sample(kinds.image, smooth, in.uv)
                            + sample(kinds.layers, smooth, in.uv, in.layer)
                            + sample(kinds.volume, smooth, in.direction)
                            + sample(kinds.cube, smooth, in.direction)
                            + sample(kinds.cubes, smooth, in.direction, 1));

    auto sources = Sources {shader.builder.graph()};

    checkAll(sources.metal,
             {"texture0.sample(sampler5, uniforms.u0)",
              "texture1.sample(sampler5, uniforms.u0, uint(uniforms.u2))",
              "texture2.sample(sampler5, uniforms.u1)",
              "texture3.sample(sampler5, uniforms.u1)",
              "texture4.sample(sampler5, uniforms.u1, uint(1))",
              "sampler sampler5 [[sampler(5)]]"});

    checkAll(sources.hlsl,
             {"texture0.Sample(sampler5, uniforms.u0)",
              "texture1.Sample(sampler5, float3(uniforms.u0, float(uniforms.u2)))",
              "texture2.Sample(sampler5, uniforms.u1)",
              "texture3.Sample(sampler5, uniforms.u1)",
              "texture4.Sample(sampler5, float4(uniforms.u1, float(1)))",
              "SamplerState sampler5 : register(s13);"});

    checkAll(sources.glsl,
             {"uniform texture2D textureImage0;",
              "uniform texture2DArray textureImage1;",
              "uniform texture3D textureImage2;",
              "uniform textureCube textureImage3;",
              "uniform textureCubeArray textureImage4;",
              "texture(sampler2D(textureImage0, sampler5), uniforms.u0)",
              "texture(sampler2DArray(textureImage1, sampler5), "
              "vec3(uniforms.u0, float(uniforms.u2)))",
              "texture(sampler3D(textureImage2, sampler5), uniforms.u1)",
              "texture(samplerCube(textureImage3, sampler5), uniforms.u1)",
              "texture(samplerCubeArray(textureImage4, sampler5), "
              "vec4(uniforms.u1, float(1)))"});

    expectGlslCompiles(shader.builder.graph());
};

auto tSampleDepth = test(
    "SamplerBindingCodegen/sampleAndCompareThroughSamplersOnEveryDepthKind") = []
{
    auto shader = Fragment {};
    auto kinds = DepthKinds {shader.builder};
    const auto& in = shader.in;

    auto plainReads = widen(sample(kinds.plane, kinds.plain, in.uv),
                            sample(kinds.planes, kinds.plain, in.uv, in.layer),
                            sample(kinds.cube, kinds.plain, in.direction),
                            sample(kinds.cubes, kinds.plain, in.direction, 1));
    auto compared = widen(
        sampleCompare(kinds.plane, kinds.shadow, in.uv, in.reference),
        sampleCompare(kinds.planes, kinds.shadow, in.uv, in.layer, in.reference),
        sampleCompare(kinds.cube, kinds.shadow, in.direction, 0.5f),
        sampleCompare(kinds.cubes, kinds.shadow, in.direction, 1, in.reference));

    shader.builder.fragment(plainReads + compared);

    auto sources = Sources {shader.builder.graph()};

    checkAll(sources.metal,
             {"texture0.sample(sampler4, uniforms.u0)",
              "texture1.sample(sampler4, uniforms.u0, uint(uniforms.u2))",
              "texture2.sample(sampler4, uniforms.u1)",
              "texture3.sample(sampler4, uniforms.u1, uint(1))",
              "texture0.sample_compare(sampler5, uniforms.u0, uniforms.u5)",
              "texture1.sample_compare(sampler5, uniforms.u0, uint(uniforms.u2), "
              "uniforms.u5)",
              "texture2.sample_compare(sampler5, uniforms.u1, 0.5)",
              "texture3.sample_compare(sampler5, uniforms.u1, uint(1), "
              "uniforms.u5)"});

    checkAll(sources.hlsl,
             {"SamplerState sampler4 : register(s12);",
              "SamplerComparisonState sampler5 : register(s13);",
              "texture0.Sample(sampler4, uniforms.u0)",
              "texture2.Sample(sampler4, uniforms.u1)",
              "texture0.SampleCmpLevelZero(sampler5, uniforms.u0, uniforms.u5)",
              "texture1.SampleCmpLevelZero(sampler5, float3(uniforms.u0, "
              "float(uniforms.u2)), uniforms.u5)",
              "texture3.SampleCmpLevelZero(sampler5, float4(uniforms.u1, "
              "float(1)), uniforms.u5)"});

    checkAll(sources.glsl,
             {"uniform sampler sampler4;",
              "uniform samplerShadow sampler5;",
              "texture(sampler2D(textureImage0, sampler4), uniforms.u0).r",
              "texture(samplerCube(textureImage2, sampler4), uniforms.u1).r",
              "texture(sampler2DShadow(textureImage0, sampler5), "
              "vec3(uniforms.u0, uniforms.u5))",
              "texture(sampler2DArrayShadow(textureImage1, sampler5), "
              "vec4(uniforms.u0, float(uniforms.u2), uniforms.u5))",
              "texture(samplerCubeShadow(textureImage2, sampler5), "
              "vec4(uniforms.u1, 0.5))",
              "texture(samplerCubeArrayShadow(textureImage3, sampler5), "
              "vec4(uniforms.u1, float(1)), uniforms.u5)"});

    expectGlslCompiles(shader.builder.graph());
};

auto tLevelBiasGrad =
    test("SamplerBindingCodegen/levelBiasAndGradThroughASampler") = []
{
    auto shader = Fragment {};
    auto kinds = ColourKinds {shader.builder};
    const auto& in = shader.in;
    const auto& smooth = kinds.smooth;

    shader.builder.fragment(
        sampleLevel(kinds.image, smooth, in.uv, in.level)
        + sampleLevel(kinds.cubes, smooth, in.direction, in.layer, 2.f)
        + sampleBias(kinds.volume, smooth, in.direction, in.level)
        + sampleBias(kinds.layers, smooth, in.uv, 1, 0.5f)
        + sampleGrad(kinds.image, smooth, in.uv, in.ddx, in.ddy)
        + sampleGrad(kinds.cube, smooth, in.direction, in.ddx3, in.ddy3)
        + sampleGrad(kinds.layers, smooth, in.uv, in.layer, in.ddx, in.ddy));

    auto sources = Sources {shader.builder.graph()};

    checkAll(sources.metal,
             {"texture0.sample(sampler5, uniforms.u0, level(uniforms.u3))",
              "texture4.sample(sampler5, uniforms.u1, uint(uniforms.u2), "
              "level(2.0))",
              "texture2.sample(sampler5, uniforms.u1, bias(uniforms.u3))",
              "texture1.sample(sampler5, uniforms.u0, uint(1), bias(0.5))",
              "texture0.sample(sampler5, uniforms.u0, gradient2d(uniforms.u6, "
              "uniforms.u7))",
              "texture3.sample(sampler5, uniforms.u1, gradientcube(uniforms.u8, "
              "uniforms.u9))",
              "texture1.sample(sampler5, uniforms.u0, uint(uniforms.u2), "
              "gradient2d(uniforms.u6, uniforms.u7))"});

    checkAll(sources.hlsl,
             {"texture0.SampleLevel(sampler5, uniforms.u0, uniforms.u3)",
              "texture4.SampleLevel(sampler5, float4(uniforms.u1, "
              "float(uniforms.u2)), 2.0)",
              "texture2.SampleBias(sampler5, uniforms.u1, uniforms.u3)",
              "texture1.SampleBias(sampler5, float3(uniforms.u0, float(1)), 0.5)",
              "texture0.SampleGrad(sampler5, uniforms.u0, uniforms.u6, "
              "uniforms.u7)",
              "texture3.SampleGrad(sampler5, uniforms.u1, uniforms.u8, "
              "uniforms.u9)"});

    checkAll(sources.glsl,
             {"textureLod(sampler2D(textureImage0, sampler5), uniforms.u0, "
              "uniforms.u3)",
              "textureLod(samplerCubeArray(textureImage4, sampler5), "
              "vec4(uniforms.u1, float(uniforms.u2)), 2.0)",
              "texture(sampler3D(textureImage2, sampler5), uniforms.u1, "
              "uniforms.u3)",
              "texture(sampler2DArray(textureImage1, sampler5), "
              "vec3(uniforms.u0, float(1)), 0.5)",
              "textureGrad(sampler2D(textureImage0, sampler5), uniforms.u0, "
              "uniforms.u6, uniforms.u7)",
              "textureGrad(samplerCube(textureImage3, sampler5), uniforms.u1, "
              "uniforms.u8, uniforms.u9)"});

    expectGlslCompiles(shader.builder.graph());
};

auto tGathers =
    test("SamplerBindingCodegen/gatherAndGatherCompareThroughSamplers") = []
{
    auto shader = Fragment {};
    auto& builder = shader.builder;
    auto image = builder.texture();
    auto cubes = builder.cubeTextureArray();
    auto plane = builder.depthTexture();
    auto planes = builder.depthTextureArray();
    auto plain = builder.sampler();
    auto shadow = builder.comparisonSampler();
    const auto& in = shader.in;

    builder.fragment(gather(image, plain, in.uv, 2)
                     + gather(cubes, plain, in.direction, in.layer, 1)
                     + gather(plane, plain, in.uv) + gather(planes, plain, in.uv, 1)
                     + gatherCompare(plane, shadow, in.uv, in.reference)
                     + gatherCompare(planes, shadow, in.uv, in.layer, 0.25f));

    auto sources = Sources {builder.graph()};

    checkAll(sources.metal,
             {"texture0.gather(sampler4, uniforms.u0, int2(0), component::z)",
              "texture1.gather(sampler4, uniforms.u1, uint(uniforms.u2), "
              "component::y)",
              "texture2.gather(sampler4, uniforms.u0)",
              "texture3.gather(sampler4, uniforms.u0, uint(1))",
              "texture2.gather_compare(sampler5, uniforms.u0, uniforms.u5)",
              "texture3.gather_compare(sampler5, uniforms.u0, uint(uniforms.u2), "
              "0.25)"});

    checkAll(sources.hlsl,
             {"texture0.GatherBlue(sampler4, uniforms.u0)",
              "texture1.GatherGreen(sampler4, float4(uniforms.u1, "
              "float(uniforms.u2)))",
              "texture2.Gather(sampler4, uniforms.u0)",
              "texture3.Gather(sampler4, float3(uniforms.u0, float(1)))",
              "texture2.GatherCmp(sampler5, uniforms.u0, uniforms.u5)",
              "texture3.GatherCmp(sampler5, float3(uniforms.u0, "
              "float(uniforms.u2)), 0.25)"});

    checkAll(sources.glsl,
             {"textureGather(sampler2D(textureImage0, sampler4), uniforms.u0, 2)",
              "textureGather(samplerCubeArray(textureImage1, sampler4), "
              "vec4(uniforms.u1, float(uniforms.u2)), 1)",
              "textureGather(sampler2D(textureImage2, sampler4), uniforms.u0)",
              "textureGather(sampler2DArray(textureImage3, sampler4), "
              "vec3(uniforms.u0, float(1)))",
              "textureGather(sampler2DShadow(textureImage2, sampler5), "
              "uniforms.u0, uniforms.u5)",
              "textureGather(sampler2DArrayShadow(textureImage3, sampler5), "
              "vec3(uniforms.u0, float(uniforms.u2)), 0.25)"});

    expectGlslCompiles(builder.graph());
};

// The any-stage forms in a kernel: the sampler is a kernel parameter on
// Metal, the same s register on HLSL, and a binding in the compute range.
struct KernelSamples
{
    KernelSamples()
        : image(builder.texture())
        , depth(builder.depthTextureArray())
        , smooth(builder.sampler())
        , shadow(builder.comparisonSampler())
        , output(builder.outputBuffer())
    {
        auto i = builder.threadId();

        auto colour = sampleLevel(image, smooth, in.uv, in.level)
                      + sampleGrad(image, smooth, in.uv, in.ddx, in.ddy)
                      + gather(image, smooth, in.uv, 3)
                      + gatherCompare(depth, shadow, in.uv, 1, in.reference);

        builder.write(
            output,
            i,
            colour.x() + sampleLevel(depth, smooth, in.uv, in.layer, 0)
                + sampleCompareLevel(depth, shadow, in.uv, in.layer, in.reference));
    }

    ShaderBuilder builder;
    Arguments in {builder};
    Texture2D image;
    TextureDepth2DArray depth;
    SamplerBinding smooth;
    ComparisonSamplerBinding shadow;
    OutputBuffer output;
};

std::string glslSamplerDeclaration(int slot, const char* type)
{
    return Strings::format("layout(set = 0, binding = {}) uniform {} sampler{};",
                           vulkanComputeTextureBinding(slot),
                           type,
                           slot);
}

auto tKernel = test("SamplerBindingCodegen/aKernelSamplesThroughSamplers") = []
{
    auto kernel = KernelSamples {};
    const auto& graph = kernel.builder.graph();
    auto sources = Sources {graph};

    check(graph.isCompute());

    checkAll(sources.metal,
             {"sampler sampler2 [[sampler(2)]],",
              "sampler sampler3 [[sampler(3)]],",
              "texture0.sample(sampler2, uniforms.u0, level(uniforms.u3))",
              "texture0.sample(sampler2, uniforms.u0, gradient2d(uniforms.u6, "
              "uniforms.u7))",
              "texture0.gather(sampler2, uniforms.u0, int2(0), component::w)",
              "texture1.gather_compare(sampler3, uniforms.u0, uint(1), "
              "uniforms.u5)",
              "texture1.sample(sampler2, uniforms.u0, uint(uniforms.u2), "
              "level(float(0)))",
              "texture1.sample_compare(sampler3, uniforms.u0, uint(uniforms.u2), "
              "uniforms.u5, level(0))"});

    checkAll(sources.hlsl,
             {"SamplerState sampler2 : register(s10);",
              "SamplerComparisonState sampler3 : register(s11);",
              "texture0.SampleLevel(sampler2, uniforms.u0, uniforms.u3)",
              "texture0.SampleGrad(sampler2, uniforms.u0, uniforms.u6, "
              "uniforms.u7)",
              "texture0.GatherAlpha(sampler2, uniforms.u0)",
              "texture1.GatherCmp(sampler3, float3(uniforms.u0, float(1)), "
              "uniforms.u5)",
              "texture1.SampleCmpLevelZero(sampler3, float3(uniforms.u0, "
              "float(uniforms.u2)), uniforms.u5)"});
    check(!contains(sources.hlsl, "texture2"));

    check(contains(sources.glsl, glslSamplerDeclaration(2, "sampler")));
    check(contains(sources.glsl, glslSamplerDeclaration(3, "samplerShadow")));
    checkAll(sources.glsl,
             {"textureLod(sampler2D(textureImage0, sampler2), uniforms.u0, "
              "uniforms.u3)",
              "textureGather(sampler2D(textureImage0, sampler2), uniforms.u0, 3)",
              "textureGather(sampler2DArrayShadow(textureImage1, sampler3), "
              "vec3(uniforms.u0, float(1)), uniforms.u5)",
              "textureLod(sampler2DArray(textureImage1, sampler2), "
              "vec3(uniforms.u0, float(uniforms.u2)), float(0)).r"});

    expectGlslCompiles(graph);
};

// Members take their slots in declaration order, a sampler's from the texture
// counter like everything else that binds beside a texture.
struct MemberKernel final : ComputeKernel
{
    MemberKernel() { compile(); }

    void define() override
    {
        auto i = threadId();
        auto uv = float2(toFloat(i), 0.5f);

        write(output,
              i,
              sampleLevel(image, smooth, uv, 0.f).x()
                  + sampleCompareLevel(depth, shadow, uv, 0.5f));
    }

    Uniform<Texture2D> image;
    Uniform<SamplerBinding> smooth;
    Uniform<TextureDepth2D> depth;
    Uniform<ComparisonSamplerBinding> shadow;
    Uniform<OutputBuffer> output;

    EACP_SHADER(image, smooth, depth, shadow, output)
};

auto tMembers =
    test("SamplerBindingCodegen/membersTakeSlotsFromTheTextureCounter") = []
{
    auto kernel = MemberKernel {};
    const auto& graph = kernel.graph();

    check(graph.textureCount() == 4);
    check(graph.textureAccess(1) == TextureAccess::Sampler);
    check(!graph.isComparisonSampler(1));
    check(graph.isComparisonSampler(3));
    check(kernel.smooth.slot == 1);
    check(kernel.shadow.slot == 3);

    auto metal = emitMetal(graph);
    check(contains(metal, "texture0.sample(sampler1, "));
    check(contains(metal, "texture2.sample_compare(sampler3, "));

    expectGlslCompiles(graph);
};

// The forms every shader had before samplers were bindings, pinned whole: a
// sampler slot beside them changes none of their text.
struct BakedShader
{
    BakedShader()
    {
        auto& builder = shader.builder;
        const auto& in = shader.in;
        auto image = builder.texture({TextureFilter::Linear,
                                      TextureAddressMode::Repeat,
                                      TextureCompare::None});
        auto layers = builder.textureArray();
        auto shadow = builder.depthTexture({TextureFilter::Linear,
                                            TextureAddressMode::Clamp,
                                            TextureCompare::LessEqual});
        auto cube = builder.cubeTexture();

        builder.fragment(sample(image, in.uv) + sample(layers, in.uv, in.layer)
                         + sampleLevel(cube, in.direction, in.level)
                         + gather(image, in.uv, 1)
                         + float4(sampleCompare(shadow, in.uv, in.reference),
                                  sampleCompareLevel(shadow, in.uv, 0.5f),
                                  1.f,
                                  1.f));
    }

    Fragment shader;
};

// Each one byte-for-byte what the codegen emitted before sampler bindings.
const auto bakedMetal = std::string {R"eacp(#include <metal_stdlib>
using namespace metal;

struct VertexIn
{
    float2 a0 [[attribute(0)]];
};

struct VertexOut
{
    float4 position [[position]];
};

struct Uniforms
{
    float2 u0;
    float3 u1;
    int u2;
    float u3;
    int u4;
    float u5;
    float2 u6;
    float2 u7;
    float3 u8;
    float3 u9;
};

vertex VertexOut vertexMain(VertexIn input [[stage_in]])
{
    VertexOut output;
    output.position = float4(input.a0, 0.0, 1.0);
    return output;
}

fragment float4 fragmentMain(VertexOut input [[stage_in]],
    constant Uniforms& uniforms [[buffer(16)]],
    texture2d<float> texture0 [[texture(0)]],
    sampler sampler0 [[sampler(0)]],
    texture2d_array<float> texture1 [[texture(1)]],
    sampler sampler1 [[sampler(1)]],
    depth2d<float> texture2 [[texture(2)]],
    sampler sampler2 [[sampler(2)]],
    texturecube<float> texture3 [[texture(3)]],
    sampler sampler3 [[sampler(3)]])
{
    return ((((texture0.sample(sampler0, uniforms.u0) + texture1.sample(sampler1, uniforms.u0, uint(uniforms.u2))) + texture3.sample(sampler3, uniforms.u1, level(uniforms.u3))) + texture0.gather(sampler0, uniforms.u0, int2(0), component::y)) + float4(texture2.sample_compare(sampler2, uniforms.u0, uniforms.u5), texture2.sample_compare(sampler2, uniforms.u0, 0.5, level(0)), 1.0, 1.0));
}
)eacp"};

const auto bakedHlsl = std::string {R"eacp(struct VertexIn
{
    float2 a0 : TEXCOORD0;
};

struct VertexOut
{
    float4 position : SV_Position;
};

struct Uniforms
{
    float2 u0;
    float3 u1;
    float pad0;
    int u2;
    float u3;
    int u4;
    float u5;
    float2 u6;
    float2 u7;
    float3 u8;
    float3 u9;
};

cbuffer UniformsCB : register(b0)
{
    Uniforms uniforms;
};

Texture2D texture0 : register(t0);
Texture2DArray texture1 : register(t1);
Texture2D<float> texture2 : register(t2);
TextureCube texture3 : register(t3);
SamplerState samplerConfig0 : register(s0);
SamplerState samplerConfig3 : register(s3);
SamplerComparisonState samplerConfig6 : register(s6);

VertexOut vertexMain(VertexIn input)
{
    VertexOut output;
    output.position = float4(input.a0, 0.0, 1.0);
    return output;
}

float4 fragmentMain(VertexOut input) : SV_Target
{
    return ((((texture0.Sample(samplerConfig3, uniforms.u0) + texture1.Sample(samplerConfig0, float3(uniforms.u0, float(uniforms.u2)))) + texture3.SampleLevel(samplerConfig0, uniforms.u1, uniforms.u3)) + texture0.GatherGreen(samplerConfig3, uniforms.u0)) + float4(texture2.SampleCmpLevelZero(samplerConfig6, uniforms.u0, uniforms.u5), texture2.SampleCmpLevelZero(samplerConfig6, uniforms.u0, 0.5), 1.0, 1.0));
}
)eacp"};

const auto bakedGlsl = std::string {R"eacp(#version 450

layout(std140, set = 0, binding = 0) uniform Uniforms
{
    vec2 u0;
    vec3 u1;
    float pad0;
    int u2;
    float u3;
    int u4;
    float u5;
    vec2 u6;
    vec2 u7;
    vec3 u8;
    vec3 u9;
} uniforms;

layout(set = 0, binding = 8) uniform sampler2D texture0;
layout(set = 0, binding = 9) uniform sampler2DArray texture1;
layout(set = 0, binding = 10) uniform sampler2DShadow texture2;
layout(set = 0, binding = 11) uniform samplerCube texture3;

#ifdef EACP_VERTEX
layout(location = 0) in vec2 attr0;

void main()
{
    gl_Position = vec4(attr0, 0.0, 1.0);
}
#endif

#ifdef EACP_FRAGMENT
layout(location = 0) out vec4 fragColor;

void main()
{
    fragColor = ((((texture(texture0, uniforms.u0) + texture(texture1, vec3(uniforms.u0, float(uniforms.u2)))) + textureLod(texture3, uniforms.u1, uniforms.u3)) + textureGather(texture0, uniforms.u0, 1)) + vec4(texture(texture2, vec3(uniforms.u0, uniforms.u5)), texture(texture2, vec3(uniforms.u0, 0.5)), 1.0, 1.0));
}
#endif
)eacp"};

auto tBakedUnchanged =
    test("SamplerBindingCodegen/bakedSamplingEmitsWhatItAlwaysDid") = []
{
    auto baked = BakedShader {};
    auto sources = Sources {baked.shader.builder.graph()};

    check(sources.metal == bakedMetal, sources.metal);
    check(sources.hlsl == bakedHlsl, sources.hlsl);
    check(sources.glsl == bakedGlsl, sources.glsl);

    expectGlslCompiles(baked.shader.builder.graph());
};

std::string withoutOnce(std::string source, const std::string& text)
{
    const auto at = source.find(text);

    if (at != std::string::npos)
        source.erase(at, text.size());

    return source;
}

auto tBakedBesideASampler =
    test("SamplerBindingCodegen/aSamplerSlotOnlyAddsItsDeclaration") = []
{
    auto baked = BakedShader {};
    baked.shader.builder.sampler();
    auto sources = Sources {baked.shader.builder.graph()};

    check(withoutOnce(sources.metal, ",\n    sampler sampler4 [[sampler(4)]]")
              == bakedMetal,
          sources.metal);
    check(withoutOnce(sources.hlsl, "SamplerState sampler4 : register(s12);\n")
              == bakedHlsl,
          sources.hlsl);
    check(withoutOnce(sources.glsl,
                      "layout(set = 0, binding = 12) uniform sampler sampler4;\n")
              == bakedGlsl,
          sources.glsl);
};

auto tCpuRefuses = test("SamplerBindingCodegen/theCpuExecutorRefusesSamplers") = []
{
    auto kernel = KernelSamples {};
    auto executor = CpuCompute::Executor {kernel.builder.graph()};

    check(!executor.isValid());
    check(contains(executor.reason(), "sampler"));
};
} // namespace SamplerBindingCodegenTests

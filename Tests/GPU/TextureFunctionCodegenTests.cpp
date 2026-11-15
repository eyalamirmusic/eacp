#include "CodegenCommon.h"

#include <eacp/GPU/Codegen/TextureFunctions.h>

#include <initializer_list>
#include <string>

// WGSL's texture functions and the kinds the EDSL gained for them - Texture3D,
// TextureCubeArray, TextureDepthCube, TextureDepthCubeArray - as text in all
// three dialects, each GLSL source compiled wherever eacp-spirv is built. The
// arguments are uniforms so every call prints the same names in every dialect;
// what the hardware does with them is TextureFunctionDeviceTests'.

using namespace nano;
using namespace eacp;
using namespace eacp::GPU;

namespace TextureFunctionCodegenTests
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

// Five colour kinds at slots 0 to 4.
struct ColourKinds
{
    explicit ColourKinds(ShaderBuilder& builder)
        : image(builder.texture())
        , layers(builder.textureArray())
        , volume(builder.texture3D())
        , cube(builder.cubeTexture())
        , cubes(builder.cubeTextureArray())
    {
    }

    Texture2D image;
    Texture2DArray layers;
    Texture3D volume;
    TextureCube cube;
    TextureCubeArray cubes;
};

// Four depth kinds at slots 0 to 3, comparing or not.
struct DepthKinds
{
    DepthKinds(ShaderBuilder& builder, TextureSampling sampling)
        : plane(builder.depthTexture(sampling))
        , planes(builder.depthTextureArray(sampling))
        , cube(builder.depthCubeTexture(sampling))
        , cubes(builder.depthCubeTextureArray(sampling))
    {
    }

    TextureDepth2D plane;
    TextureDepth2DArray planes;
    TextureDepthCube cube;
    TextureDepthCubeArray cubes;
};

constexpr auto comparing = TextureSampling {
    TextureFilter::Linear, TextureAddressMode::Clamp, TextureCompare::LessEqual};

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
} // namespace

auto tDeclarations =
    test("TextureFunctionCodegen/theNewKindsDeclareTheirOwnTypes") = []
{
    auto shader = Fragment {};
    auto& builder = shader.builder;
    auto volume = builder.texture3D();
    auto cubes = builder.cubeTextureArray();
    auto depthCube = builder.depthCubeTexture();
    auto depthCubes = builder.depthCubeTextureArray();
    auto shadowCube = builder.depthCubeTexture(comparing);
    auto shadowCubes = builder.depthCubeTextureArray(comparing);
    const auto& in = shader.in;

    builder.fragment(
        sample(volume, in.direction) + sample(cubes, in.direction, in.layer)
        + widen(sample(depthCube, in.direction),
                sample(depthCubes, in.direction, 1),
                sampleCompare(shadowCube, in.direction, in.reference),
                sampleCompare(shadowCubes, in.direction, in.layer, in.reference)));

    auto sources = Sources {builder.graph()};

    checkAll(sources.metal,
             {"texture3d<float> texture0 [[texture(0)]]",
              "texturecube_array<float> texture1 [[texture(1)]]",
              "depthcube<float> texture2 [[texture(2)]]",
              "depthcube_array<float> texture3 [[texture(3)]]",
              "depthcube<float> texture4 [[texture(4)]]",
              "depthcube_array<float> texture5 [[texture(5)]]",
              "texture0.sample(sampler0, uniforms.u1)",
              "texture1.sample(sampler1, uniforms.u1, uint(uniforms.u2))",
              "texture2.sample(sampler2, uniforms.u1)",
              "texture3.sample(sampler3, uniforms.u1, uint(1))",
              "texture4.sample_compare(sampler4, uniforms.u1, uniforms.u5)",
              "texture5.sample_compare(sampler5, uniforms.u1, uint(uniforms.u2), "
              "uniforms.u5)"});

    checkAll(sources.hlsl,
             {"Texture3D texture0 : register(t0);",
              "TextureCubeArray texture1 : register(t1);",
              "TextureCube<float> texture2 : register(t2);",
              "TextureCubeArray<float> texture3 : register(t3);",
              "SamplerComparisonState samplerConfig",
              "texture0.Sample(samplerConfig0, uniforms.u1)",
              "texture1.Sample(samplerConfig0, float4(uniforms.u1, "
              "float(uniforms.u2)))",
              "texture2.Sample(samplerConfig0, uniforms.u1)",
              "texture3.Sample(samplerConfig0, float4(uniforms.u1, float(1)))",
              "texture4.SampleCmpLevelZero(",
              "texture5.SampleCmpLevelZero("});

    checkAll(sources.glsl,
             {"uniform sampler3D texture0;",
              "uniform samplerCubeArray texture1;",
              "uniform samplerCube texture2;",
              "uniform samplerCubeArray texture3;",
              "uniform samplerCubeShadow texture4;",
              "uniform samplerCubeArrayShadow texture5;",
              "texture(texture0, uniforms.u1)",
              "texture(texture1, vec4(uniforms.u1, float(uniforms.u2)))",
              "texture(texture2, uniforms.u1).r",
              "texture(texture3, vec4(uniforms.u1, float(1))).r",
              "texture(texture4, vec4(uniforms.u1, uniforms.u5))",
              "texture(texture5, vec4(uniforms.u1, float(uniforms.u2)), "
              "uniforms.u5)"});

    expectGlslCompiles(builder.graph());
};

auto tLevels = test("TextureFunctionCodegen/sampleLevelOnEveryColourKind") = []
{
    auto shader = Fragment {};
    auto kinds = ColourKinds {shader.builder};
    const auto& in = shader.in;

    shader.builder.fragment(sampleLevel(kinds.image, in.uv, in.level)
                            + sampleLevel(kinds.layers, in.uv, in.layer, in.level)
                            + sampleLevel(kinds.volume, in.direction, in.level)
                            + sampleLevel(kinds.cube, in.direction, 2.f)
                            + sampleLevel(kinds.cubes, in.direction, 1, in.level));

    auto sources = Sources {shader.builder.graph()};

    checkAll(
        sources.metal,
        {"texture0.sample(sampler0, uniforms.u0, level(uniforms.u3))",
         "texture1.sample(sampler1, uniforms.u0, uint(uniforms.u2), "
         "level(uniforms.u3))",
         "texture2.sample(sampler2, uniforms.u1, level(uniforms.u3))",
         "texture3.sample(sampler3, uniforms.u1, level(2.0))",
         "texture4.sample(sampler4, uniforms.u1, uint(1), level(uniforms.u3))"});

    checkAll(sources.hlsl,
             {"texture0.SampleLevel(samplerConfig0, uniforms.u0, uniforms.u3)",
              "texture1.SampleLevel(samplerConfig0, float3(uniforms.u0, "
              "float(uniforms.u2)), uniforms.u3)",
              "texture2.SampleLevel(samplerConfig0, uniforms.u1, uniforms.u3)",
              "texture3.SampleLevel(samplerConfig0, uniforms.u1, 2.0)",
              "texture4.SampleLevel(samplerConfig0, float4(uniforms.u1, float(1)), "
              "uniforms.u3)"});

    checkAll(sources.glsl,
             {"textureLod(texture0, uniforms.u0, uniforms.u3)",
              "textureLod(texture1, vec3(uniforms.u0, float(uniforms.u2)), "
              "uniforms.u3)",
              "textureLod(texture2, uniforms.u1, uniforms.u3)",
              "textureLod(texture3, uniforms.u1, 2.0)",
              "textureLod(texture4, vec4(uniforms.u1, float(1)), uniforms.u3)"});

    expectGlslCompiles(shader.builder.graph());
};

// WGSL's depth levels are integers; the dialects take a float.
auto tDepthLevels = test("TextureFunctionCodegen/sampleLevelOnEveryDepthKind") = []
{
    auto shader = Fragment {};
    auto kinds = DepthKinds {shader.builder, {}};
    const auto& in = shader.in;

    shader.builder.fragment(
        widen(sampleLevel(kinds.plane, in.uv, in.index),
              sampleLevel(kinds.planes, in.uv, in.layer, 0),
              sampleLevel(kinds.cube, in.direction, in.index),
              sampleLevel(kinds.cubes, in.direction, in.layer, in.index)));

    auto sources = Sources {shader.builder.graph()};

    checkAll(sources.metal,
             {"texture0.sample(sampler0, uniforms.u0, level(float(uniforms.u4)))",
              "texture1.sample(sampler1, uniforms.u0, uint(uniforms.u2), "
              "level(float(0)))",
              "texture2.sample(sampler2, uniforms.u1, level(float(uniforms.u4)))",
              "texture3.sample(sampler3, uniforms.u1, uint(uniforms.u2), "
              "level(float(uniforms.u4)))"});

    checkAll(sources.hlsl,
             {"texture0.SampleLevel(samplerConfig0, uniforms.u0, "
              "float(uniforms.u4))",
              "texture2.SampleLevel(samplerConfig0, uniforms.u1, "
              "float(uniforms.u4))",
              "texture3.SampleLevel(samplerConfig0, float4(uniforms.u1, "
              "float(uniforms.u2)), float(uniforms.u4))"});

    checkAll(sources.glsl,
             {"textureLod(texture0, uniforms.u0, float(uniforms.u4)).r",
              "textureLod(texture1, vec3(uniforms.u0, float(uniforms.u2)), "
              "float(0)).r",
              "textureLod(texture3, vec4(uniforms.u1, float(uniforms.u2)), "
              "float(uniforms.u4)).r"});

    expectGlslCompiles(shader.builder.graph());
};

auto tBias = test("TextureFunctionCodegen/sampleBiasOnEveryColourKind") = []
{
    auto shader = Fragment {};
    auto kinds = ColourKinds {shader.builder};
    const auto& in = shader.in;

    shader.builder.fragment(sampleBias(kinds.image, in.uv, in.level)
                            + sampleBias(kinds.layers, in.uv, in.layer, in.level)
                            + sampleBias(kinds.volume, in.direction, 1.5f)
                            + sampleBias(kinds.cube, in.direction, in.level)
                            + sampleBias(kinds.cubes, in.direction, 1, in.level));

    auto sources = Sources {shader.builder.graph()};

    checkAll(sources.metal,
             {"texture0.sample(sampler0, uniforms.u0, bias(uniforms.u3))",
              "texture1.sample(sampler1, uniforms.u0, uint(uniforms.u2), "
              "bias(uniforms.u3))",
              "texture2.sample(sampler2, uniforms.u1, bias(1.5))",
              "texture3.sample(sampler3, uniforms.u1, bias(uniforms.u3))",
              "texture4.sample(sampler4, uniforms.u1, uint(1), bias(uniforms.u3))"});

    checkAll(sources.hlsl,
             {"texture0.SampleBias(samplerConfig0, uniforms.u0, uniforms.u3)",
              "texture1.SampleBias(samplerConfig0, float3(uniforms.u0, "
              "float(uniforms.u2)), uniforms.u3)",
              "texture2.SampleBias(samplerConfig0, uniforms.u1, 1.5)",
              "texture4.SampleBias(samplerConfig0, float4(uniforms.u1, float(1)), "
              "uniforms.u3)"});

    checkAll(sources.glsl,
             {"texture(texture0, uniforms.u0, uniforms.u3)",
              "texture(texture1, vec3(uniforms.u0, float(uniforms.u2)), "
              "uniforms.u3)",
              "texture(texture2, uniforms.u1, 1.5)",
              "texture(texture4, vec4(uniforms.u1, float(1)), uniforms.u3)"});

    expectGlslCompiles(shader.builder.graph());
};

auto tGrad = test("TextureFunctionCodegen/sampleGradPicksTheGradientShape") = []
{
    auto shader = Fragment {};
    auto kinds = ColourKinds {shader.builder};
    const auto& in = shader.in;

    shader.builder.fragment(
        sampleGrad(kinds.image, in.uv, in.ddx, in.ddy)
        + sampleGrad(kinds.layers, in.uv, in.layer, in.ddx, in.ddy)
        + sampleGrad(kinds.volume, in.direction, in.ddx3, in.ddy3)
        + sampleGrad(kinds.cube, in.direction, in.ddx3, in.ddy3)
        + sampleGrad(kinds.cubes, in.direction, in.layer, in.ddx3, in.ddy3));

    auto sources = Sources {shader.builder.graph()};

    checkAll(sources.metal,
             {"texture0.sample(sampler0, uniforms.u0, gradient2d(uniforms.u6, "
              "uniforms.u7))",
              "texture1.sample(sampler1, uniforms.u0, uint(uniforms.u2), "
              "gradient2d(uniforms.u6, uniforms.u7))",
              "texture2.sample(sampler2, uniforms.u1, gradient3d(uniforms.u8, "
              "uniforms.u9))",
              "texture3.sample(sampler3, uniforms.u1, gradientcube(uniforms.u8, "
              "uniforms.u9))",
              "texture4.sample(sampler4, uniforms.u1, uint(uniforms.u2), "
              "gradientcube(uniforms.u8, uniforms.u9))"});

    checkAll(sources.hlsl,
             {"texture0.SampleGrad(samplerConfig0, uniforms.u0, uniforms.u6, "
              "uniforms.u7)",
              "texture2.SampleGrad(samplerConfig0, uniforms.u1, uniforms.u8, "
              "uniforms.u9)",
              "texture4.SampleGrad(samplerConfig0, float4(uniforms.u1, "
              "float(uniforms.u2)), uniforms.u8, uniforms.u9)"});

    checkAll(sources.glsl,
             {"textureGrad(texture0, uniforms.u0, uniforms.u6, uniforms.u7)",
              "textureGrad(texture1, vec3(uniforms.u0, float(uniforms.u2)), "
              "uniforms.u6, uniforms.u7)",
              "textureGrad(texture3, uniforms.u1, uniforms.u8, uniforms.u9)"});

    expectGlslCompiles(shader.builder.graph());
};

// Level 0 on Metal by name; HLSL's comparison is always the level-zero one, and
// GLSL's texture() on a one-level depth view reads level 0 in any stage.
auto tCompareLevel =
    test("TextureFunctionCodegen/sampleCompareLevelOnEveryDepthKind") = []
{
    auto shader = Fragment {};
    auto kinds = DepthKinds {shader.builder, comparing};
    const auto& in = shader.in;

    shader.builder.fragment(widen(
        sampleCompareLevel(kinds.plane, in.uv, in.reference),
        sampleCompareLevel(kinds.planes, in.uv, in.layer, in.reference),
        sampleCompareLevel(kinds.cube, in.direction, 0.5f),
        sampleCompareLevel(kinds.cubes, in.direction, in.layer, in.reference)));

    auto sources = Sources {shader.builder.graph()};

    checkAll(sources.metal,
             {"texture0.sample_compare(sampler0, uniforms.u0, uniforms.u5, "
              "level(0))",
              "texture1.sample_compare(sampler1, uniforms.u0, uint(uniforms.u2), "
              "uniforms.u5, level(0))",
              "texture2.sample_compare(sampler2, uniforms.u1, 0.5, level(0))",
              "texture3.sample_compare(sampler3, uniforms.u1, uint(uniforms.u2), "
              "uniforms.u5, level(0))"});

    checkAll(sources.hlsl,
             {"texture0.SampleCmpLevelZero(samplerConfig6, uniforms.u0, "
              "uniforms.u5)",
              "texture1.SampleCmpLevelZero(samplerConfig6, float3(uniforms.u0, "
              "float(uniforms.u2)), uniforms.u5)",
              "texture2.SampleCmpLevelZero(samplerConfig6, uniforms.u1, 0.5)",
              "texture3.SampleCmpLevelZero(samplerConfig6, float4(uniforms.u1, "
              "float(uniforms.u2)), uniforms.u5)"});

    checkAll(sources.glsl,
             {"uniform sampler2DShadow texture0;",
              "uniform sampler2DArrayShadow texture1;",
              "texture(texture0, vec3(uniforms.u0, uniforms.u5))",
              "texture(texture1, vec4(uniforms.u0, float(uniforms.u2), "
              "uniforms.u5))",
              "texture(texture2, vec4(uniforms.u1, 0.5))",
              "texture(texture3, vec4(uniforms.u1, float(uniforms.u2)), "
              "uniforms.u5)"});

    expectGlslCompiles(shader.builder.graph());
};

auto tGather = test("TextureFunctionCodegen/gatherReadsTheComponentAsked") = []
{
    auto shader = Fragment {};
    auto kinds = ColourKinds {shader.builder};
    auto depth = shader.builder.depthTexture();
    auto depthCubes = shader.builder.depthCubeTextureArray();
    const auto& in = shader.in;

    shader.builder.fragment(
        gather(kinds.image, in.uv) + gather(kinds.layers, in.uv, in.layer, 1)
        + gather(kinds.cube, in.direction, 2)
        + gather(kinds.cubes, in.direction, 1, 3) + gather(depth, in.uv)
        + gather(depthCubes, in.direction, in.layer));

    auto sources = Sources {shader.builder.graph()};

    checkAll(sources.metal,
             {"texture0.gather(sampler0, uniforms.u0, int2(0), component::x)",
              "texture1.gather(sampler1, uniforms.u0, uint(uniforms.u2), int2(0), "
              "component::y)",
              "texture3.gather(sampler3, uniforms.u1, component::z)",
              "texture4.gather(sampler4, uniforms.u1, uint(1), component::w)",
              "texture5.gather(sampler5, uniforms.u0)",
              "texture6.gather(sampler6, uniforms.u1, uint(uniforms.u2))"});

    checkAll(sources.hlsl,
             {"texture0.GatherRed(samplerConfig0, uniforms.u0)",
              "texture1.GatherGreen(samplerConfig0, float3(uniforms.u0, "
              "float(uniforms.u2)))",
              "texture3.GatherBlue(samplerConfig0, uniforms.u1)",
              "texture4.GatherAlpha(samplerConfig0, float4(uniforms.u1, float(1)))",
              "texture5.Gather(samplerConfig0, uniforms.u0)",
              "texture6.Gather(samplerConfig0, float4(uniforms.u1, "
              "float(uniforms.u2)))"});

    checkAll(sources.glsl,
             {"textureGather(texture0, uniforms.u0, 0)",
              "textureGather(texture1, vec3(uniforms.u0, float(uniforms.u2)), 1)",
              "textureGather(texture3, uniforms.u1, 2)",
              "textureGather(texture4, vec4(uniforms.u1, float(1)), 3)",
              "textureGather(texture5, uniforms.u0)",
              "textureGather(texture6, vec4(uniforms.u1, float(uniforms.u2)))"});

    expectGlslCompiles(shader.builder.graph());
};

auto tGatherCompare =
    test("TextureFunctionCodegen/gatherCompareOnEveryDepthKind") = []
{
    auto shader = Fragment {};
    auto kinds = DepthKinds {shader.builder, comparing};
    const auto& in = shader.in;

    shader.builder.fragment(
        gatherCompare(kinds.plane, in.uv, in.reference)
        + gatherCompare(kinds.planes, in.uv, in.layer, in.reference)
        + gatherCompare(kinds.cube, in.direction, in.reference)
        + gatherCompare(kinds.cubes, in.direction, 0, in.reference));

    auto sources = Sources {shader.builder.graph()};

    checkAll(sources.metal,
             {"texture0.gather_compare(sampler0, uniforms.u0, uniforms.u5)",
              "texture1.gather_compare(sampler1, uniforms.u0, uint(uniforms.u2), "
              "uniforms.u5)",
              "texture2.gather_compare(sampler2, uniforms.u1, uniforms.u5)",
              "texture3.gather_compare(sampler3, uniforms.u1, uint(0), "
              "uniforms.u5)"});

    checkAll(sources.hlsl,
             {"texture0.GatherCmp(samplerConfig6, uniforms.u0, uniforms.u5)",
              "texture1.GatherCmp(samplerConfig6, float3(uniforms.u0, "
              "float(uniforms.u2)), uniforms.u5)",
              "texture3.GatherCmp(samplerConfig6, float4(uniforms.u1, float(0)), "
              "uniforms.u5)"});

    checkAll(sources.glsl,
             {"textureGather(texture0, uniforms.u0, uniforms.u5)",
              "textureGather(texture1, vec3(uniforms.u0, float(uniforms.u2)), "
              "uniforms.u5)",
              "textureGather(texture2, uniforms.u1, uniforms.u5)",
              "textureGather(texture3, vec4(uniforms.u1, float(0)), uniforms.u5)"});

    expectGlslCompiles(shader.builder.graph());
};

// The loads, the queries and the any-stage samples in a kernel, where every
// one of them is legal.
struct KernelReads
{
    KernelReads()
        : image(builder.texture())
        , layers(builder.textureArray())
        , volume(builder.texture3D())
        , cubes(builder.cubeTextureArray())
        , depth(builder.depthTexture())
        , output(builder.outputBuffer())
        , texel(builder.uniform<Int2>())
        , voxel(builder.uniform<Int3>())
    {
        auto i = builder.threadId();
        auto unsignedTexel = toUInt(texel.x());

        auto colour = load(image, texel, in.index) + load(layers, texel, 2, 1)
                      + load(volume, voxel, in.index)
                      + sampleLevel(layers, in.uv, in.layer, 0.f)
                      + sampleGrad(volume, in.direction, in.ddx3, in.ddy3)
                      + gather(cubes, in.direction, in.layer, 1);

        auto size = dimensions(image) + dimensions(layers, in.index)
                    + dimensions(cubes, unsignedTexel);
        auto volumeSize = dimensions(volume, 1);
        auto counts = levelCount(image) + levelCount(depth) + layerCount(layers)
                      + layerCount(cubes) + volumeSize.z();

        builder.write(output,
                      i,
                      colour.x() + toFloat(size.x() + counts)
                          + load(depth, texel, 0));
    }

    ShaderBuilder builder;
    Arguments in {builder};
    Texture2D image;
    Texture2DArray layers;
    Texture3D volume;
    TextureCubeArray cubes;
    TextureDepth2D depth;
    OutputBuffer output;
    Int2 texel;
    Int3 voxel;
};

auto tLoads = test("TextureFunctionCodegen/loadAddressesLevelLayerAndSlice") = []
{
    auto kernel = KernelReads {};
    auto sources = Sources {kernel.builder.graph()};

    checkAll(sources.metal,
             {"texture0.read(uint2(uniforms.u10), uint(uniforms.u4))",
              "texture1.read(uint2(uniforms.u10), uint(2), uint(1))",
              "texture2.read(uint3(uniforms.u11), uint(uniforms.u4))",
              "texture4.read(uint2(uniforms.u10), uint(0))"});

    checkAll(sources.hlsl,
             {"texture0.Load(int3(uniforms.u10, uniforms.u4))",
              "texture1.Load(int4(uniforms.u10, 2, 1))",
              "texture2.Load(int4(uniforms.u11, uniforms.u4))",
              "texture4.Load(int3(uniforms.u10, 0))"});

    checkAll(sources.glsl,
             {"texelFetch(texture0, uniforms.u10, uniforms.u4)",
              "texelFetch(texture1, ivec3(uniforms.u10, 2), 1)",
              "texelFetch(texture2, uniforms.u11, uniforms.u4)",
              "texelFetch(texture4, uniforms.u10, 0).r"});

    expectGlslCompiles(kernel.builder.graph());
};

// HLSL's GetDimensions answers through out-parameters, so each queried kind
// gets one helper returning width, height, depth or layers, and levels.
auto tQueries = test("TextureFunctionCodegen/queriesAreExpressionsEverywhere") = []
{
    auto kernel = KernelReads {};
    auto sources = Sources {kernel.builder.graph()};

    checkAll(sources.metal,
             {"uint2(texture0.get_width(), texture0.get_height())",
              "uint2(texture1.get_width(uint(uniforms.u4)), "
              "texture1.get_height(uint(uniforms.u4)))",
              "uint3(texture2.get_width(uint(1)), texture2.get_height(uint(1)), "
              "texture2.get_depth(uint(1)))",
              "texture0.get_num_mip_levels()",
              "texture4.get_num_mip_levels()",
              "texture1.get_array_size()",
              "texture3.get_array_size()"});

    checkAll(sources.hlsl,
             {"uint4 eacpTextureInfo2D(Texture2D t, uint level)",
              "    uint width, height, levels;\n"
              "    t.GetDimensions(level, width, height, levels);\n"
              "    return uint4(width, height, 1, levels);",
              "uint4 eacpTextureInfo2DArray(Texture2DArray t, uint level)",
              "    t.GetDimensions(level, width, height, layers, levels);\n"
              "    return uint4(width, height, layers, levels);",
              "uint4 eacpTextureInfo3D(Texture3D t, uint level)",
              "    return uint4(width, height, depth, levels);",
              "uint4 eacpTextureInfoCubeArray(TextureCubeArray t, uint level)",
              "uint4 eacpTextureInfoDepth2D(Texture2D<float> t, uint level)",
              "eacpTextureInfo2D(texture0, 0u).xy",
              "eacpTextureInfo2DArray(texture1, uint(uniforms.u4)).xy",
              "eacpTextureInfo3D(texture2, uint(1)).xyz",
              "eacpTextureInfo2D(texture0, 0u).w",
              "eacpTextureInfoDepth2D(texture4, 0u).w",
              "eacpTextureInfo2DArray(texture1, 0u).z",
              "eacpTextureInfoCubeArray(texture3, 0u).z"});

    checkAll(sources.glsl,
             {"uvec2(textureSize(texture0, 0).xy)",
              "uvec2(textureSize(texture1, uniforms.u4).xy)",
              "uvec3(textureSize(texture2, 1))",
              "uint(textureQueryLevels(texture0))",
              "uint(textureQueryLevels(texture4))",
              "uint(textureSize(texture1, 0).z)",
              "uint(textureSize(texture3, 0).z)"});

    expectGlslCompiles(kernel.builder.graph());
};

auto tNoHelperUnasked =
    test("TextureFunctionCodegen/aShaderThatQueriesNothingHasNoHelper") = []
{
    auto shader = Fragment {};
    auto image = shader.builder.texture();
    shader.builder.fragment(sampleLevel(image, shader.in.uv, 0.f));

    check(!contains(emitHlsl(shader.builder.graph()), "eacpTextureInfo"));
};

auto tCpuRefuses =
    test("TextureFunctionCodegen/theCpuExecutorRefusesTextureFunctions") = []
{
    auto builder = ShaderBuilder {};
    auto output = builder.outputBuffer();
    auto image = builder.texture3D();
    auto i = builder.threadId();
    builder.write(output, i, toFloat(dimensions(image).x()));

    auto executor = CpuCompute::Executor {builder.graph()};
    check(!executor.isValid());
    check(contains(executor.reason(), "texture"));
};
} // namespace TextureFunctionCodegenTests

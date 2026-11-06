#include "CodegenCommon.h"

#include <initializer_list>
#include <string>

using namespace nano;
using namespace eacp;
using namespace eacp::GPU;

namespace VertexStatementCodegenTests
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

// A vertex stage with every expression form a statement-free one has - an
// attribute, uniforms, a storage buffer, a constant array, both vertex
// built-ins, a value read twice - beside a fragment stage with statements and
// a texture.
struct StatementFreeShader
{
    StatementFreeShader()
    {
        auto position = builder.vertexInput<Float2>();
        auto weight = builder.vertexInput<Float>();
        auto scale = builder.uniform<Float>();
        auto tint = builder.uniform<Float4>();
        auto offsets = builder.inputBuffer();
        auto image = builder.texture();
        auto steps = builder.array(builder.constant(0.25f), builder.constant(0.5f));

        auto lean =
            offsets[builder.vertexId()] * scale + steps[toInt(builder.instanceId())];
        auto moved = position * scale + float2(lean, lean);

        builder.position(float4(moved, weight, 1.f));

        auto carried = builder.varying(moved);
        auto instance = builder.varying(builder.instanceId());

        auto counter = builder.var(0.f);
        auto advance = [&] { counter += 1.f; };
        builder.loop(counter < 3.f, advance);

        builder.fragment(sample(image, carried) * tint * counter.get()
                         + float4(toFloat(instance), 0.f, 0.f, 0.f));
    }

    ShaderBuilder builder;
};

// Each one byte-for-byte what the codegen emitted before vertex statements.
const auto statementFreeMetal = std::string {R"eacp(#include <metal_stdlib>
using namespace metal;

struct VertexIn
{
    float2 a0 [[attribute(0)]];
    float a1 [[attribute(1)]];
};

struct VertexOut
{
    float4 position [[position]];
    float2 v0;
    uint v1 [[flat]];
};

struct Uniforms
{
    float u0;
    float4 u1;
};

vertex VertexOut vertexMain(VertexIn input [[stage_in]], uint vertexId [[vertex_id]], uint instanceId [[instance_id]], constant Uniforms& uniforms [[buffer(16)]],
    device const float* buffer0 [[buffer(24)]])
{
    const float a0[2] = {0.25, 0.5};
    float t0 = ((buffer0[vertexId] * uniforms.u0) + a0[int(instanceId)]);
    float2 t1 = ((input.a0 * uniforms.u0) + float2(t0, t0));
    VertexOut output;
    output.position = float4(t1, input.a1, 1.0);
    output.v0 = t1;
    output.v1 = instanceId;
    return output;
}

fragment float4 fragmentMain(VertexOut input [[stage_in]],
    constant Uniforms& uniforms [[buffer(16)]],
    texture2d<float> texture0 [[texture(0)]],
    sampler sampler0 [[sampler(0)]])
{
    float v0 = 0.0;
    while ((v0 < 3.0))
    {
        v0 = (v0 + 1.0);
    }
    return (((texture0.sample(sampler0, input.v0) * uniforms.u1) * v0) + float4(float(input.v1), 0.0, 0.0, 0.0));
}
)eacp"};

const auto statementFreeHlsl = std::string {R"eacp(struct VertexIn
{
    float2 a0 : TEXCOORD0;
    float a1 : TEXCOORD1;
};

struct VertexOut
{
    float4 position : SV_Position;
    float2 v0 : TEXCOORD0;
    nointerpolation uint v1 : TEXCOORD1;
};

struct Uniforms
{
    float u0;
    float4 u1;
};

cbuffer UniformsCB : register(b0)
{
    Uniforms uniforms;
};

Texture2D texture0 : register(t0);
SamplerState samplerConfig0 : register(s0);

StructuredBuffer<float> buffer0 : register(t8);

VertexOut vertexMain(VertexIn input, uint vertexId : SV_VertexID, uint instanceId : SV_InstanceID)
{
    const float a0[2] = {0.25, 0.5};
    float t0 = ((buffer0[vertexId] * uniforms.u0) + a0[int(instanceId)]);
    float2 t1 = ((input.a0 * uniforms.u0) + float2(t0, t0));
    VertexOut output;
    output.position = float4(t1, input.a1, 1.0);
    output.v0 = t1;
    output.v1 = instanceId;
    return output;
}

float4 fragmentMain(VertexOut input) : SV_Target
{
    float v0 = 0.0;
    while ((v0 < 3.0))
    {
        v0 = (v0 + 1.0);
    }
    return (((texture0.Sample(samplerConfig0, input.v0) * uniforms.u1) * v0) + float4(float(input.v1), 0.0, 0.0, 0.0));
}
)eacp"};

const auto statementFreeGlsl = std::string {R"eacp(#version 450

layout(std140, set = 0, binding = 0) uniform Uniforms
{
    float u0;
    vec4 u1;
} uniforms;

layout(set = 0, binding = 8) uniform sampler2D texture0;

layout(std430, set = 0, binding = 24) readonly buffer Buffer0
{
    float buffer0[];
};

#ifdef EACP_VERTEX
layout(location = 0) in vec2 attr0;
layout(location = 1) in float attr1;
layout(location = 0) out vec2 vary0;
layout(location = 1) flat out uint vary1;

void main()
{
    float a0[2] = {0.25, 0.5};
    float t0 = ((buffer0[uint(gl_VertexIndex)] * uniforms.u0) + a0[int(uint(gl_InstanceIndex))]);
    vec2 t1 = ((attr0 * uniforms.u0) + vec2(t0, t0));
    gl_Position = vec4(t1, attr1, 1.0);
    vary0 = t1;
    vary1 = uint(gl_InstanceIndex);
}
#endif

#ifdef EACP_FRAGMENT
layout(location = 0) in vec2 vary0;
layout(location = 1) flat in uint vary1;
layout(location = 0) out vec4 fragColor;

void main()
{
    float v0 = 0.0;
    while ((v0 < 3.0))
    {
        v0 = (v0 + 1.0);
    }
    fragColor = (((texture(texture0, vary0) * uniforms.u1) * v0) + vec4(float(vary1), 0.0, 0.0, 0.0));
}
#endif
)eacp"};
} // namespace

auto tStatementFree =
    test("VertexStatementCodegen/aStatementFreeVertexEmitsWhatItAlwaysDid") = []
{
    auto shader = StatementFreeShader {};
    auto sources = Sources {shader.builder.graph()};

    check(sources.metal == statementFreeMetal, sources.metal);
    check(sources.hlsl == statementFreeHlsl, sources.hlsl);
    check(sources.glsl == statementFreeGlsl, sources.glsl);
    check(
        buildShader(shader.builder.graph(), ShaderBackend::Metal).vertexTextureSlots
        == 0u);

    expectGlslCompiles(shader.builder.graph());
};

namespace
{
// The vertex function's text and the fragment function's, split where the
// fragment one starts.
struct Stages
{
    explicit Stages(const std::string& source)
    {
        const auto glsl = contains(source, "#ifdef EACP_VERTEX");
        const auto vertexAt =
            source.find(glsl ? "#ifdef EACP_VERTEX" : "vertexMain(");
        const auto fragmentAt =
            source.find(glsl ? "#ifdef EACP_FRAGMENT" : "fragmentMain(");

        vertex = source.substr(vertexAt, fragmentAt - vertexAt);
        fragment = source.substr(fragmentAt);
    }

    std::string vertex;
    std::string fragment;
};

bool precedes(const std::string& source,
              std::string_view first,
              std::string_view then)
{
    const auto firstAt = source.find(first);
    const auto thenAt = source.find(then);
    return firstAt != std::string::npos && thenAt != std::string::npos
           && firstAt < thenAt;
}

void checkAll(const std::string& source,
              std::initializer_list<const char*> expected,
              const std::source_location& location = std::source_location::current())
{
    for (const auto* text: expected)
        check(contains(source, text), text, location);
}

Float4 white(ShaderBuilder& builder)
{
    return float4(builder.constant(1.f), 1.f, 1.f, 1.f);
}

// A column raised by summing `step` until `count` steps or a height of 4,
// all of it in the vertex stage; the uniforms are read by its statements
// alone.
struct RaisedColumn
{
    RaisedColumn()
    {
        auto position = builder.vertexInput<Float2>();
        auto count = builder.uniform<Int>();
        auto step = builder.uniform<Float>();

        auto raise = [&]
        {
            auto height = builder.var(0.f);
            auto taken = builder.var(0);

            auto climb = [&]
            {
                height += step;
                taken += 1;

                auto stop = [&] { builder.breakLoop(); };
                builder.ifThen(height.get() > 4.f, stop);
            };

            builder.loop(taken.get() < count, climb);
            builder.position(float4(position, height.get(), 1.f));
        };

        builder.vertexStage(raise);
        builder.fragment(float4(builder.constant(1.f), 0.f, 0.f, 1.f));
    }

    ShaderBuilder builder;
};
} // namespace

auto tVertexLoop =
    test("VertexStatementCodegen/varLoopAndIfAreEmittedIntoTheVertexFunction") = []
{
    auto column = RaisedColumn {};
    const auto& graph = column.builder.graph();
    auto sources = Sources {graph};

    auto metal = Stages {sources.metal};
    checkAll(metal.vertex,
             {"constant Uniforms& uniforms [[buffer(16)]]",
              "    float v0 = 0.0;\n"
              "    int v1 = 0;\n"
              "    while ((v1 < uniforms.u0))\n"
              "    {\n"
              "        v0 = (v0 + uniforms.u1);\n"
              "        v1 = (v1 + 1);\n"
              "        if ((v0 > 4.0))\n"
              "        {\n"
              "            break;\n"
              "        }\n"
              "    }\n",
              "output.position = float4(input.a0, v0, 1.0);"});
    check(precedes(metal.vertex, "while", "VertexOut output;"));
    check(!contains(metal.fragment, "v0"));
    check(!contains(metal.fragment, "uniforms"));

    auto hlsl = Stages {sources.hlsl};
    checkAll(hlsl.vertex,
             {"float v0 = 0.0;",
              "int v1 = 0;",
              "while ((v1 < uniforms.u0))",
              "break;",
              "output.position = float4(input.a0, v0, 1.0);"});
    check(!contains(hlsl.fragment, "v0"));

    auto glsl = Stages {sources.glsl};
    checkAll(glsl.vertex,
             {"float v0 = 0.0;",
              "int v1 = 0;",
              "while ((v1 < uniforms.u0))",
              "break;",
              "gl_Position = vec4(attr0, v0, 1.0);"});
    check(precedes(glsl.vertex, "while", "gl_Position"));
    check(!contains(glsl.fragment, "v0"));

    check(vertexReadsUniforms(graph));
    check(!fragmentReadsUniforms(graph));

    expectGlslCompiles(graph);
};

auto tBothStages =
    test("VertexStatementCodegen/eachStageEmitsOnlyItsOwnStatements") = []
{
    auto builder = ShaderBuilder {};
    auto position = builder.vertexInput<Float2>();
    auto bias = builder.uniform<Float>();
    auto carried = Float2 {};

    auto vertexBody = [&]
    {
        auto lifted = builder.var(position);
        auto lift = [&] { lifted += float2(bias, bias); };
        builder.ifThen(position.x() > 0.f, lift);

        builder.position(float4(lifted.get(), 0.f, 1.f));
        carried = builder.varying(lifted.get());
    };

    builder.vertexStage(vertexBody);

    auto shade = builder.var(carried.x());
    auto darken = [&] { shade *= 0.5f; };
    builder.ifThen(carried.y() > 0.f, darken);
    builder.fragment(float4(shade.get(), 0.f, 0.f, 1.f));

    const auto& graph = builder.graph();
    check(stagesReadOnlyTheirOwnVariables(graph));
    check(graph.isVertexVariable(0));
    check(!graph.isVertexVariable(1));

    auto sources = Sources {graph};

    for (const auto* source: {&sources.metal, &sources.hlsl, &sources.glsl})
    {
        auto stages = Stages {*source};
        check(contains(stages.vertex, "v0 = "), *source);
        check(!contains(stages.vertex, "v1"), *source);
        check(contains(stages.fragment, "v1 = "), *source);
        check(!contains(stages.fragment, " v0 = "), *source);
        check(!contains(stages.fragment, "(v0"), *source);
    }

    check(vertexReadsUniforms(graph));
    check(!fragmentReadsUniforms(graph));

    expectGlslCompiles(graph);
};

auto tVertexBuffer =
    test("VertexStatementCodegen/aBufferReadInAVertexStatementIsDeclared") = []
{
    auto builder = ShaderBuilder {};
    auto position = builder.vertexInput<Float2>();
    auto heights = builder.inputBuffer();

    auto vertexBody = [&]
    {
        auto total = builder.var(0.f);
        auto index = builder.var(0u);
        auto sum = [&]
        {
            total += heights[index.get()];
            index += 1u;
        };

        builder.loop(index.get() < 4u, sum);
        builder.position(float4(position, total.get(), 1.f));
    };

    builder.vertexStage(vertexBody);
    builder.fragment(white(builder));

    auto metal = Stages {emitMetal(builder.graph())};
    check(contains(metal.vertex, "device const float* buffer0 [[buffer(24)]]"),
          metal.vertex);
    check(contains(metal.vertex, "(v0 + buffer0[v1])"), metal.vertex);
    check(!contains(metal.fragment, "buffer0"));

    expectGlslCompiles(builder.graph());
};

namespace
{
// A grid displaced by a height map read at level 0 - through the texture's
// own sampling, through a sampler binding, and as a texel load - and shaded
// by the same map in the fragment stage.
struct DisplacedGrid
{
    DisplacedGrid()
    {
        auto position = builder.vertexInput<Float2>();

        auto displace = [&]
        {
            auto uv = position * 0.5f + 0.5f;
            auto height = builder.var(sampleLevel(heights, uv, 0.f).x());
            height += sampleLevel(heights, smooth, uv, 0.f).y();
            height += sample(heights, uv, 0.f).z();
            height += load(heights, int2(builder.integer(0), 0), 0).w();
            builder.position(float4(position, height.get(), 1.f));
        };

        builder.vertexStage(displace);
        builder.fragment(sample(heights, builder.varying(position)));
    }

    ShaderBuilder builder;
    Texture2D heights = builder.texture();
    SamplerBinding smooth = builder.sampler();
    Texture2D unread = builder.texture();
};
} // namespace

auto tVertexTextures =
    test("VertexStatementCodegen/aVertexTextureReadDeclaresItInTheVertexStage") = []
{
    auto grid = DisplacedGrid {};
    const auto& graph = grid.builder.graph();

    check(!vertexUsesImplicitDerivatives(graph));
    check(vertexTextureSlots(graph) == 0b011u);

    auto generated = buildShader(graph, ShaderBackend::Metal);
    check(generated.vertexReadsTexture(0));
    check(generated.vertexReadsTexture(1));
    check(!generated.vertexReadsTexture(2));

    auto sources = Sources {graph};

    auto metal = Stages {sources.metal};
    checkAll(metal.vertex,
             {"texture2d<float> texture0 [[texture(0)]]",
              "sampler sampler0 [[sampler(0)]]",
              "sampler sampler1 [[sampler(1)]]",
              "texture0.sample(sampler0, t0, level(0.0))",
              "texture0.sample(sampler1, t0, level(0.0))",
              "texture0.read("});
    check(!contains(metal.vertex, "texture2 "));
    checkAll(metal.fragment,
             {"texture2d<float> texture0 [[texture(0)]]",
              "texture2d<float> texture2 [[texture(2)]]",
              "texture0.sample(sampler0, input.v0)"});

    auto hlsl = Stages {sources.hlsl};
    checkAll(hlsl.vertex,
             {"texture0.SampleLevel(samplerConfig0, t0, 0.0)",
              "texture0.SampleLevel(sampler1, t0, 0.0)",
              "texture0.Load("});
    check(!contains(hlsl.vertex, ".Sample("));

    auto glsl = Stages {sources.glsl};
    checkAll(glsl.vertex,
             {"textureLod(texture0, t0, 0.0)",
              "textureLod(sampler2D(textureImage0, sampler1), t0, 0.0)",
              "texelFetch(texture0, "});

    expectGlslCompiles(graph);
};

auto tExplicitForms =
    test("VertexStatementCodegen/everyExplicitLevelFormReadsInTheVertexStage") = []
{
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

    auto read = [&]
    {
        auto uv = position * 0.5f + 0.5f;
        auto sum = builder.var(sampleGrad(colour, uv, uv * 0.f, uv * 0.f));
        sum += sampleLevel(layers, uv, 1, 0.f);
        sum += sampleLevel(volume, float3(uv, 0.5f), 0.f);
        sum += gather(colour, uv, 2);
        sum += sampleCompareLevel(shadow, uv, 0.5f);
        sum += sampleCompareLevel(plain, compare, uv, 0.5f);
        sum += gatherCompare(plain, compare, uv, 0.5f);
        sum += toFloat(dimensions(colour).x());
        builder.position(float4(position, sum.get().x(), 1.f));
    };

    builder.vertexStage(read);
    builder.fragment(white(builder));

    const auto& graph = builder.graph();
    check(!vertexUsesImplicitDerivatives(graph));
    check(vertexTextureSlots(graph) == 0b111111u);

    auto metal = Stages {emitMetal(graph)};
    checkAll(metal.vertex,
             {"texture2d_array<float> texture1 [[texture(1)]]",
              "texture3d<float> texture2 [[texture(2)]]",
              "depth2d<float> texture3 [[texture(3)]]",
              "sampler sampler4 [[sampler(4)]]",
              "depth2d<float> texture5 [[texture(5)]]",
              "gradient2d(",
              ".gather(",
              ".sample_compare(sampler3, ",
              ".gather_compare(sampler4, ",
              ".get_width("});

    auto hlsl = Stages {emitHlsl(graph)};
    checkAll(
        hlsl.vertex,
        {".SampleGrad(", ".GatherBlue(", ".SampleCmpLevelZero(", ".GatherCmp("});

    auto glsl = Stages {emitGlsl(graph)};
    checkAll(glsl.vertex,
             {"textureGrad(", "textureGather(", "textureSize(texture0, 0)"});

    expectGlslCompiles(graph);
};

namespace
{
// The position of a vertex whose stage reaches `value`; the predicate under
// test is the whole check, so nothing is emitted.
struct VertexReading
{
    template <typename Reach>
    explicit VertexReading(Reach&& reach)
    {
        position = builder.vertexInput<Float2>();
        builder.position(float4(position, reach(*this), 1.f));
        builder.fragment(white(builder));
    }

    ShaderBuilder builder;
    Float2 position;
};
} // namespace

auto tDerivativesRefused =
    test("VertexStatementCodegen/implicitDerivativesAreRefusedInTheVertexStage") = []
{
    auto plainSample = [](VertexReading& in)
    { return sample(in.builder.texture(), in.position).x(); };
    auto bias = [](VertexReading& in)
    { return sampleBias(in.builder.texture(), in.position, 1.f).x(); };
    auto throughSampler = [](VertexReading& in)
    {
        auto image = in.builder.texture();
        return sample(image, in.builder.sampler(), in.position).x();
    };
    auto compare = [](VertexReading& in)
    {
        auto shadow = in.builder.depthTexture({TextureFilter::Linear,
                                               TextureAddressMode::Clamp,
                                               TextureCompare::LessEqual});
        return sampleCompare(shadow, in.position, in.position.x());
    };
    auto layered = [](VertexReading& in)
    { return sample(in.builder.textureArray(), in.position, 0).x(); };
    auto derivative = [](VertexReading& in) { return dfdx(in.position.x()); };
    auto width = [](VertexReading& in) { return fwidth(in.position.y()); };

    check(
        vertexUsesImplicitDerivatives(VertexReading {plainSample}.builder.graph()));
    check(vertexUsesImplicitDerivatives(VertexReading {bias}.builder.graph()));
    check(vertexUsesImplicitDerivatives(
        VertexReading {throughSampler}.builder.graph()));
    check(vertexUsesImplicitDerivatives(VertexReading {compare}.builder.graph()));
    check(vertexUsesImplicitDerivatives(VertexReading {layered}.builder.graph()));
    check(vertexUsesImplicitDerivatives(VertexReading {derivative}.builder.graph()));
    check(vertexUsesImplicitDerivatives(VertexReading {width}.builder.graph()));

    auto level = [](VertexReading& in)
    { return sample(in.builder.texture(), in.position, 0.f).x(); };
    check(!vertexUsesImplicitDerivatives(VertexReading {level}.builder.graph()));
};

auto tFragmentStillDerives =
    test("VertexStatementCodegen/aFragmentDerivativeIsNoVertexOne") = []
{
    auto builder = ShaderBuilder {};
    auto position = builder.vertexInput<Float2>();
    auto image = builder.texture();
    builder.position(float4(position, 0.f, 1.f));

    auto carried = builder.varying(position);
    builder.fragment(sample(image, carried) + dfdx(carried.x()));

    check(!vertexUsesImplicitDerivatives(builder.graph()));
    check(vertexTextureSlots(builder.graph()) == 0u);
};

auto tArrayElements =
    test("VertexStatementCodegen/aUniformReadOnlyInAnArrayElementIsDeclared") = []
{
    auto builder = ShaderBuilder {};
    auto position = builder.vertexInput<Float2>();
    auto lift = builder.uniform<Float>();
    auto shades = builder.inputBuffer();

    auto heights = builder.array(lift * 0.5f, lift * 2.f);
    auto tints = builder.array(shades[0u], shades[1u]);

    auto vertexBody = [&]
    {
        auto height = builder.var(heights[toInt(builder.vertexId() % 2u)]);
        builder.position(float4(position, height.get(), 1.f));
    };

    builder.vertexStage(vertexBody);
    auto shade = tints[toInt(builder.sampleIndex())];
    builder.fragment(float4(shade, shade, shade, 1.f));

    const auto& graph = builder.graph();
    check(vertexReadsUniforms(graph));
    check(!fragmentReadsUniforms(graph));

    auto metal = Stages {emitMetal(graph)};
    checkAll(metal.vertex,
             {"constant Uniforms& uniforms [[buffer(16)]]",
              "const float a0[2] = {(uniforms.u0 * 0.5), (uniforms.u0 * 2.0)};"});
    checkAll(metal.fragment,
             {"device const float* buffer0 [[buffer(24)]]",
              "const float a1[2] = {buffer0[0u], buffer0[1u]};"});
    check(!contains(metal.fragment, "uniforms"), metal.fragment);
    check(!contains(metal.vertex, "buffer0"), metal.vertex);

    check(buildShader(graph, ShaderBackend::Metal).vertexReadsUniforms);

    expectGlslCompiles(graph);
};

auto tCrossingRefused =
    test("VertexStatementCodegen/aVariableIsReadOnlyByItsOwnStage") = []
{
    {
        auto builder = ShaderBuilder {};
        auto position = builder.vertexInput<Float2>();
        auto leaked = Float {};

        auto vertexBody = [&]
        {
            auto height = builder.var(1.f);
            leaked = height.get();
            builder.position(float4(position, height.get(), 1.f));
        };

        builder.vertexStage(vertexBody);
        builder.fragment(float4(leaked, leaked, leaked, 1.f));

        check(!stagesReadOnlyTheirOwnVariables(builder.graph()));
    }

    {
        auto builder = ShaderBuilder {};
        auto position = builder.vertexInput<Float2>();
        auto shade = builder.var(1.f);

        builder.position(float4(position, shade.get(), 1.f));
        builder.fragment(float4(shade.get(), 0.f, 0.f, 1.f));

        check(!stagesReadOnlyTheirOwnVariables(builder.graph()));
    }
};
} // namespace VertexStatementCodegenTests

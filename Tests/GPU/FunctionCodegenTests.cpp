#include "CodegenCommon.h"

#include <initializer_list>
#include <source_location>
#include <string>
#include <string_view>

// WGSL's user functions as the three dialects spell them: a function declared
// once with ShaderBuilder::function is defined once per source, ahead of the
// stage calling it and behind every function it calls, and called by name. MSL
// passes it the uniforms, textures, samplers and buffers its body reaches; HLSL
// and GLSL reach them as the globals they are. Calls of a function that may be
// read as a value are expressions, the rest statements. A source no stage of
// which calls a function is the source it was. The GLSL is compiled wherever
// eacp-spirv is built; the numbers are pinned by CpuComputeTests' Function/
// and on a device by FunctionDevice/.

using namespace nano;
using namespace eacp;
using namespace eacp::GPU;

namespace FunctionCodegenTests
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

void checkAll(const std::string& source,
              std::initializer_list<std::string_view> fragments,
              const std::source_location& where = std::source_location::current())
{
    for (auto fragment: fragments)
        check(has(source, fragment),
              std::string(fragment) + "\n---\n" + source,
              where);
}

void checkNone(const std::string& source,
               std::initializer_list<std::string_view> fragments,
               const std::source_location& where = std::source_location::current())
{
    for (auto fragment: fragments)
        check(!has(source, fragment),
              std::string(fragment) + "\n---\n" + source,
              where);
}

struct Particle
{
    Float3 position;
    Float mass;

    EACP_SHADER_STRUCT(Particle, position, mass)
};

// A function with an early return, called twice by another.
struct SmallKernel
{
    SmallKernel()
    {
        auto input = builder.inputBuffer();
        auto output = builder.outputBuffer();
        auto id = builder.threadId();

        auto clampUnit = builder.function<Float(Float)>(
            "clampUnit",
            [&](Float x)
            {
                auto belowZero = [&] { builder.returnEarly(builder.constant(0.f)); };
                builder.ifThen(x < 0.f, belowZero);
                return min(x, 1.f);
            });

        auto blend = builder.function<Float(Float, Float)>(
            "blend",
            [&](Float a, Float b)
            { return clampUnit(a) * 0.5f + clampUnit(b) * 0.5f; });

        builder.write(output, id, blend(input[id], input[id + 1u]));
    }

    ShaderBuilder builder;
};

constexpr auto smallMetal = std::string_view {R"golden(#include <metal_stdlib>
using namespace metal;

struct Uniforms
{
    uint count;
};

float fn_clampUnit(float v0)
{
    if ((v0 < 0.0))
    {
        return 0.0;
    }
    return min(v0, 1.0);
}

float fn_blend(float v1, float v2)
{
    return ((fn_clampUnit(v1) * 0.5) + (fn_clampUnit(v2) * 0.5));
}

kernel void computeMain(device const float* buffer0 [[buffer(0)]],
    device float* buffer1 [[buffer(1)]],
    constant Uniforms& uniforms [[buffer(16)]],
    uint gid [[thread_position_in_grid]])
{
    if (gid >= uniforms.count)
        return;
    buffer1[gid] = fn_blend(buffer0[gid], buffer0[(gid + 1u)]);
}
)golden"};

constexpr auto smallHlsl = std::string_view {R"golden(struct Uniforms
{
    uint count;
};

cbuffer UniformsCB : register(b0)
{
    Uniforms uniforms;
};

StructuredBuffer<float> buffer0 : register(t0);
RWStructuredBuffer<float> buffer1 : register(u1);

float fn_clampUnit(float v0)
{
    if ((v0 < 0.0))
    {
        return 0.0;
    }
    return min(v0, 1.0);
}

float fn_blend(float v1, float v2)
{
    return ((fn_clampUnit(v1) * 0.5) + (fn_clampUnit(v2) * 0.5));
}

[numthreads(64, 1, 1)]
void computeMain(uint3 threadId : SV_DispatchThreadID)
{
    uint gid = threadId.x;
    if (gid >= uniforms.count)
        return;
    buffer1[gid] = fn_blend(buffer0[gid], buffer0[(gid + 1u)]);
}
)golden"};

constexpr auto smallGlsl = std::string_view {R"golden(#version 450

layout(std140, set = 0, binding = 16) uniform Uniforms
{
    uint count;
} uniforms;

layout(std430, set = 0, binding = 0) readonly buffer Buffer0
{
    float buffer0[];
};
layout(std430, set = 0, binding = 1) buffer Buffer1
{
    float buffer1[];
};

layout(local_size_x = 64, local_size_y = 1, local_size_z = 1) in;

float fn_clampUnit(float v0)
{
    if ((v0 < 0.0))
    {
        return 0.0;
    }
    return min(v0, 1.0);
}

float fn_blend(float v1, float v2)
{
    return ((fn_clampUnit(v1) * 0.5) + (fn_clampUnit(v2) * 0.5));
}

void main()
{
    uint gid = gl_GlobalInvocationID.x;
    if (gid >= uniforms.count)
        return;
    buffer1[gid] = fn_blend(buffer0[gid], buffer0[(gid + 1u)]);
}
)golden"};

// One function called from both render stages, one from the fragment alone,
// reaching a uniform and a texture.
struct RenderShader
{
    RenderShader()
    {
        auto position = builder.vertexInput<Float2>();
        auto scale = builder.uniform<Float>();
        auto image = builder.texture();

        auto wave = builder.function<Float(Float)>(
            "wave", [&](Float x) { return sin(x * scale) * 0.5f + 0.5f; });

        auto tint = builder.function<Float4(Float2, Float)>(
            "tint",
            [&](Float2 uv, Float level)
            { return sampleLevel(image, uv, level) * wave(uv.x()); });

        builder.position(float4(position * wave(position.y()), 0.f, 1.f));
        auto uv = builder.varying(position);
        builder.fragment(tint(uv, wave(uv.y())));
    }

    ShaderBuilder builder;
};

constexpr auto renderMetal = std::string_view {R"golden(#include <metal_stdlib>
using namespace metal;

struct VertexIn
{
    float2 a0 [[attribute(0)]];
};

struct VertexOut
{
    float4 position [[position]];
    float2 v0;
};

struct Uniforms
{
    float u0;
};

float fn_wave(float v0, constant Uniforms& uniforms)
{
    return ((sin((v0 * uniforms.u0)) * 0.5) + 0.5);
}

float4 fn_tint(float2 v1, float v2, constant Uniforms& uniforms, texture2d<float> texture0, sampler sampler0)
{
    return (texture0.sample(sampler0, v1, level(v2)) * fn_wave((v1).x, uniforms));
}

vertex VertexOut vertexMain(VertexIn input [[stage_in]], constant Uniforms& uniforms [[buffer(16)]])
{
    VertexOut output;
    output.position = float4((input.a0 * fn_wave((input.a0).y, uniforms)), 0.0, 1.0);
    output.v0 = input.a0;
    return output;
}

fragment float4 fragmentMain(VertexOut input [[stage_in]],
    constant Uniforms& uniforms [[buffer(16)]],
    texture2d<float> texture0 [[texture(0)]],
    sampler sampler0 [[sampler(0)]])
{
    return fn_tint(input.v0, fn_wave((input.v0).y, uniforms), uniforms, texture0, sampler0);
}
)golden"};

constexpr auto renderHlsl = std::string_view {R"golden(struct VertexIn
{
    float2 a0 : TEXCOORD0;
};

struct VertexOut
{
    float4 position : SV_Position;
    float2 v0 : TEXCOORD0;
};

struct Uniforms
{
    float u0;
};

cbuffer UniformsCB : register(b0)
{
    Uniforms uniforms;
};

Texture2D texture0 : register(t0);
SamplerState samplerConfig0 : register(s0);

float fn_wave(float v0)
{
    return ((sin((v0 * uniforms.u0)) * 0.5) + 0.5);
}

float4 fn_tint(float2 v1, float v2)
{
    return (texture0.SampleLevel(samplerConfig0, v1, v2) * fn_wave((v1).x));
}

VertexOut vertexMain(VertexIn input)
{
    VertexOut output;
    output.position = float4((input.a0 * fn_wave((input.a0).y)), 0.0, 1.0);
    output.v0 = input.a0;
    return output;
}

float4 fragmentMain(VertexOut input) : SV_Target
{
    return fn_tint(input.v0, fn_wave((input.v0).y));
}
)golden"};

constexpr auto renderGlsl = std::string_view {R"golden(#version 450

layout(std140, set = 0, binding = 0) uniform Uniforms
{
    float u0;
} uniforms;

layout(set = 0, binding = 8) uniform sampler2D texture0;

#ifdef EACP_VERTEX
layout(location = 0) in vec2 attr0;
layout(location = 0) out vec2 vary0;

float fn_wave(float v0)
{
    return ((sin((v0 * uniforms.u0)) * 0.5) + 0.5);
}

void main()
{
    gl_Position = vec4((attr0 * fn_wave((attr0).y)), 0.0, 1.0);
    vary0 = attr0;
}
#endif

#ifdef EACP_FRAGMENT
layout(location = 0) in vec2 vary0;
layout(location = 0) out vec4 fragColor;

float fn_wave(float v0)
{
    return ((sin((v0 * uniforms.u0)) * 0.5) + 0.5);
}

vec4 fn_tint(vec2 v1, float v2)
{
    return (textureLod(texture0, v1, v2) * fn_wave((v1).x));
}

void main()
{
    fragColor = fn_tint(vary0, fn_wave((vary0).y));
}
#endif
)golden"};

// One function called six times, five of them through each other.
struct ManyCallsKernel
{
    ManyCallsKernel()
    {
        auto input = builder.inputBuffer();
        auto output = builder.outputBuffer();
        auto id = builder.threadId();

        auto square =
            builder.function<Float(Float)>("square", [&](Float x) { return x * x; });

        auto value = square(input[id]);

        for (auto call = 0; call < 5; ++call)
            value = square(value);

        builder.write(output, id, value);
    }

    ShaderBuilder builder;
};

// Three functions declared in order, the last calling the other two and the
// second the first; the kernel calls the last alone.
struct ChainKernel
{
    ChainKernel()
    {
        auto output = builder.outputBuffer();
        auto id = builder.threadId();

        auto inner = builder.function<Float(Float)>(
            "inner", [&](Float x) { return x + 1.f; });
        auto middle = builder.function<Float(Float)>(
            "middle", [&](Float x) { return inner(x) * 2.f; });
        auto outer = builder.function<Float(Float)>(
            "outer", [&](Float x) { return middle(x) - inner(x); });

        builder.write(output, id, outer(toFloat(id)));
    }

    ShaderBuilder builder;
};

// Returns from a loop, a switch's clause and an if, and a call in a loop's
// condition.
struct SearchKernel
{
    SearchKernel()
    {
        auto output = builder.uintOutputBuffer();
        auto id = builder.threadId();

        auto firstSquareAbove = builder.function<UInt(UInt, UInt)>(
            "firstSquareAbove",
            [&](UInt start, UInt limit)
            {
                auto at = builder.var(start);
                auto found = [&] { builder.returnEarly(at.get()); };
                auto special = [&]
                { builder.returnEarly(builder.unsignedInteger(500u)); };
                auto step = [&]
                {
                    builder.ifThen(at.get() * at.get() > limit, found);
                    builder.switchOn(at.get(), caseOf(9, special));
                    at += 1u;
                };

                builder.loop(at.get() < 12u, step);
                return builder.unsignedInteger(999u);
            });

        auto twice =
            builder.function<UInt(UInt)>("twice", [&](UInt x) { return x * 2u; });

        auto count = builder.var(0u);
        auto advance = [&] { count += 1u; };
        builder.loop(twice(count.get()) < id % 13u, advance);

        builder.write(output, id, firstSquareAbove(id % 5u, id) + count.get());
    }

    ShaderBuilder builder;
};

// A void function storing and counting; a value function reading what the
// kernel wrote, which is called as a statement.
struct StorageKernel
{
    StorageKernel()
    {
        auto input = builder.inputBuffer();
        auto first = builder.outputBuffer();
        auto counter = builder.atomicBuffer();
        auto id = builder.threadId();

        auto record = builder.function<void(UInt, Float)>(
            "record",
            [&](UInt at, Float value)
            {
                builder.write(first, at, value);
                builder.atomicAdd(counter, 0u, 1u);
                auto leave = [&] { builder.returnEarly(); };
                builder.ifThen(value > 0.5f, leave);
                builder.write(first, at, value * 2.f);
            });

        auto readBack = builder.function<Float(UInt)>(
            "readBack", [&](UInt at) { return first[at] + 1.f; });

        record(id, input[id]);
        builder.write(first, id + 1u, readBack(id));
    }

    ShaderBuilder builder;
};

// A struct in and out, f16 values and a matrix.
struct ValueKernel
{
    ValueKernel()
    {
        auto input = builder.structInputBuffer<Particle>();
        auto output = builder.structOutputBuffer<Particle>();
        auto halves = builder.halfInputBuffer();
        auto halfOutput = builder.halfOutputBuffer();
        auto turns = builder.outputBuffer();
        auto id = builder.threadId();

        auto weigh = builder.function<Particle(Particle, Float)>(
            "weigh",
            [&](Particle particle, Float k)
            { return Particle {particle.position * k, particle.mass + k}; });

        auto mixHalf = builder.function<Half(Half, Half2)>(
            "mixHalf", [&](Half h, Half2 v) { return h * v.x() + v.y(); });

        auto turn = builder.function<Float2(Float2x2, Float2)>(
            "turn", [&](Float2x2 m, Float2 v) { return m * v; });

        auto k = toFloat(id);
        builder.write(output, id, weigh(input[id], k));

        auto h = halves[id];
        builder.write(halfOutput, id, mixHalf(h, half2(h, toHalf(k))));

        auto rotation = float2x2(float2(builder.constant(0.f), 1.f),
                                 float2(builder.constant(-1.f), 0.f));
        builder.write(turns, id, turn(rotation, float2(k, 1.f)).x());
    }

    ShaderBuilder builder;
};

// The same kernel with a function declared after its body and never called,
// or without one.
struct PlainKernel
{
    explicit PlainKernel(bool declareFunction)
    {
        auto input = builder.inputBuffer();
        auto output = builder.outputBuffer();
        auto id = builder.threadId();
        auto total = builder.var(input[id]);
        auto grow = [&] { total *= 2.f; };
        builder.loop(total.get() < 10.f, grow);
        builder.write(output, id, total.get());

        if (declareFunction)
            builder.function<Float(Float)>("unused",
                                           [&](Float x) { return x * 3.f; });
    }

    ShaderBuilder builder;
};

// A function taking derivatives, called from the fragment stage, or from the
// vertex stage as well.
struct DerivativeShader
{
    explicit DerivativeShader(bool fromVertex)
    {
        auto position = builder.vertexInput<Float2>();

        auto edge = builder.function<Float(Float)>(
            "edge", [&](Float x) { return fwidth(x) * 4.f; });

        auto x = fromVertex ? edge(position.x()) : position.x();
        builder.position(float4(x, position.y(), 0.f, 1.f));
        auto carried = builder.varying(position);
        builder.fragment(float4(edge(carried.x()), 0.f, 0.f, 1.f));
    }

    ShaderBuilder builder;
};

// A struct-returning function, so a statement, called inside vertexStage and
// in the fragment stage.
struct VertexStatementShader
{
    VertexStatementShader()
    {
        auto position = builder.vertexInput<Float2>();

        auto lift = builder.function<Particle(Float2)>(
            "lift",
            [&](Float2 p) { return Particle {float3(p, 0.5f), p.x() + p.y()}; });

        auto place = [&]
        {
            auto lifted = lift(position);
            builder.position(float4(lifted.position, 1.f));
        };

        builder.vertexStage(place);
        auto carried = builder.varying(position);
        builder.fragment(float4(lift(carried).mass, 0.f, 0.f, 1.f));
    }

    ShaderBuilder builder;
};

// A void function writing a texture at a coordinate it is handed, scaled by
// an override.
struct TextureKernel
{
    TextureKernel()
    {
        auto target = builder.writableTexture();
        auto gain = builder.overrideConstant<Float>("gain", 2.f);
        auto position = builder.threadPosition();

        auto paint = builder.function<void(UInt, UInt, Float)>(
            "paint",
            [&](UInt x, UInt y, Float shade)
            { builder.write(target, x, y, float4(shade * gain, 0.f, 0.f, 1.f)); });

        paint(position.x, position.y, toFloat(position.x));
    }

    ShaderBuilder builder;
};

// A fragment function that discards, called as a statement.
struct DiscardShader
{
    DiscardShader()
    {
        auto position = builder.vertexInput<Float2>();

        auto dropTopRow =
            builder.function<void(Float)>("dropTopRow",
                                          [&](Float y)
                                          {
                                              auto drop = [&] { builder.discard(); };
                                              builder.ifThen(y < 1.f, drop);
                                          });

        builder.position(float4(position, 0.f, 1.f));
        dropTopRow(builder.fragmentCoordinate().y());
        builder.fragment(float4(builder.constant(1.f), 0.f, 0.f, 1.f));
    }

    ShaderBuilder builder;
};

auto tSmall = test("FunctionCodegen/aKernelMatchesTheGoldens") = []
{
    auto kernel = SmallKernel {};
    auto sources = Sources {kernel.builder.graph()};

    check(sources.metal == smallMetal, sources.metal);
    check(sources.hlsl == smallHlsl, sources.hlsl);
    check(sources.glsl == smallGlsl, sources.glsl);
    expectGlslCompiles(sources.glsl, true);
};

auto tRender = test("FunctionCodegen/aRenderProgramMatchesTheGoldens") = []
{
    auto shader = RenderShader {};
    auto sources = Sources {shader.builder.graph()};

    check(sources.metal == renderMetal, sources.metal);
    check(sources.hlsl == renderHlsl, sources.hlsl);
    check(sources.glsl == renderGlsl, sources.glsl);
    expectGlslCompiles(sources.glsl);
};

auto tOnce = test("FunctionCodegen/aFunctionCalledSixTimesIsDefinedOnce") = []
{
    auto kernel = ManyCallsKernel {};
    auto sources = Sources {kernel.builder.graph()};

    for (const auto* source: {&sources.metal, &sources.hlsl, &sources.glsl})
    {
        check(occurrences(*source, "float fn_square(float v0)") == 1, *source);
        check(occurrences(*source, "fn_square(") == 7, *source);
        check(occurrences(*source, "(v0 * v0)") == 1, *source);
    }

    expectGlslCompiles(sources.glsl, true);
    expectPlans(CpuCompute::Executor {kernel.builder.graph()});
};

auto tOrder = test("FunctionCodegen/calleesAreDefinedAheadOfTheirCallers") = []
{
    auto kernel = ChainKernel {};
    auto sources = Sources {kernel.builder.graph()};

    for (const auto* source: {&sources.metal, &sources.hlsl, &sources.glsl})
    {
        auto inner = source->find("float fn_inner(");
        auto middle = source->find("float fn_middle(");
        auto outer = source->find("float fn_outer(");

        check(inner != std::string::npos && inner < middle && middle < outer
                  && outer < source->find("main("),
              *source);
        check(occurrences(*source, "float fn_inner(") == 1, *source);
        check(has(*source, "return (fn_middle(v2) - fn_inner(v2));"), *source);
    }

    expectGlslCompiles(sources.glsl, true);
    expectPlans(CpuCompute::Executor {kernel.builder.graph()});
};

auto tReturns = test("FunctionCodegen/aValueReturnsFromAnyStatement") = []
{
    auto kernel = SearchKernel {};
    auto sources = Sources {kernel.builder.graph()};

    for (const auto* source: {&sources.metal, &sources.hlsl, &sources.glsl})
    {
        checkAll(*source,
                 {"uint fn_firstSquareAbove(uint v0, uint v1)",
                  "        if (((v2 * v2) > v1))\n        {\n            return v2;",
                  "        case 9u:\n            {\n                return 500u;",
                  "    return 999u;\n}",
                  "while ((fn_twice("});
    }

    expectGlslCompiles(sources.glsl, true);
    expectPlans(CpuCompute::Executor {kernel.builder.graph()});
};

auto tStorage = test("FunctionCodegen/storageIsWrittenByStatementCalls") = []
{
    auto kernel = StorageKernel {};
    const auto& graph = kernel.builder.graph();
    auto sources = Sources {graph};

    check(graph.isCalledAsStatement(0) && graph.isCalledAsStatement(1));
    check(graph.function(0).writesStorage && !graph.function(0).returnsValue);
    check(graph.function(1).readsMutableStorage && !graph.function(1).writesStorage);

    checkAll(sources.metal,
             {"void fn_record(uint v0, float v1, device float* buffer1, device "
              "atomic_uint* buffer2)",
              "float fn_readBack(uint v3, device float* buffer1)",
              "    fn_record(gid, buffer0[gid], buffer1, buffer2);\n"
              "    float v4 = fn_readBack(gid, buffer1);\n"
              "    buffer1[(gid + 1u)] = v4;"});

    for (const auto* source: {&sources.hlsl, &sources.glsl})
        checkAll(*source,
                 {"void fn_record(uint v0, float v1)",
                  "float fn_readBack(uint v3)",
                  "    fn_record(gid, buffer0[gid]);\n"
                  "    float v4 = fn_readBack(gid);\n"
                  "    buffer1[(gid + 1u)] = v4;"});

    expectGlslCompiles(sources.glsl, true);
    expectPlans(CpuCompute::Executor {graph});
};

auto tValues = test("FunctionCodegen/structsHalvesAndMatricesPassByValue") = []
{
    auto kernel = ValueKernel {};
    const auto& graph = kernel.builder.graph();
    auto sources = Sources {graph};

    check(graph.isCalledAsStatement(0), "a struct result is a statement's");
    check(!graph.isCalledAsStatement(1) && !graph.isCalledAsStatement(2));

    checkAll(sources.metal,
             {"Particle fn_weigh(Particle v0, float v1)",
              "return eacpMakeParticle((float3(v0.position) * v1), (v0.mass + v1));",
              "half fn_mixHalf(half v2, half2 v3)",
              "float2 fn_turn(float2x2 v4, float2 v5)",
              "Particle v6 = fn_weigh("});

    checkAll(sources.hlsl,
             {"Particle fn_weigh(Particle v0, float v1)",
              "float fn_mixHalf(float v2, float2 v3)",
              "return eacpRoundHalf((eacpRoundHalf((v2 * (v3).x)) + (v3).y));",
              "float2 fn_turn(float2x2 v4, float2 v5)\n{\n    return mul(v4, v5);",
              "Particle v6 = fn_weigh("});

    checkAll(sources.glsl,
             {"Particle fn_weigh(Particle v0, float v1)",
              "float16_t fn_mixHalf(float16_t v2, f16vec2 v3)",
              "vec2 fn_turn(mat2 v4, vec2 v5)",
              "Particle v6 = fn_weigh("});

    expectGlslCompiles(sources.glsl, true);
    expectPlans(CpuCompute::Executor {graph});
};

auto tUnused = test("FunctionCodegen/aFunctionNothingCallsLeavesTheSource") = []
{
    auto plain = PlainKernel {false};
    auto declared = PlainKernel {true};
    auto plainSources = Sources {plain.builder.graph()};
    auto declaredSources = Sources {declared.builder.graph()};

    check(declared.builder.graph().functionCount() == 1);
    check(declaredSources.metal == plainSources.metal, declaredSources.metal);
    check(declaredSources.hlsl == plainSources.hlsl, declaredSources.hlsl);
    check(declaredSources.glsl == plainSources.glsl, declaredSources.glsl);
    checkNone(declaredSources.metal, {"fn_unused"});
};

auto tPure = test("FunctionCodegen/aPureCallIsAValueOfItsArguments") = []
{
    auto kernel = ChainKernel {};
    const auto& graph = kernel.builder.graph();

    for (auto function = 0; function < graph.functionCount(); ++function)
    {
        check(graph.function(function).isPure);
        check(!graph.isCalledAsStatement(function));
    }

    auto storage = StorageKernel {};
    check(!storage.builder.graph().function(0).isPure);
    check(!storage.builder.graph().function(1).isPure);
};

auto tStages = test("FunctionCodegen/eachGlslStageDefinesTheFunctionsItCalls") = []
{
    auto shader = RenderShader {};
    auto glsl = emitGlsl(shader.builder.graph());
    auto fragment = glsl.find("#ifdef EACP_FRAGMENT");
    auto vertexPart = glsl.substr(0, fragment);
    auto fragmentPart = glsl.substr(fragment);

    check(has(vertexPart, "float fn_wave(") && !has(vertexPart, "fn_tint"), glsl);
    check(has(fragmentPart, "float fn_wave(") && has(fragmentPart, "vec4 fn_tint("),
          glsl);
    check(occurrences(emitMetal(shader.builder.graph()), "float fn_wave(") == 1);
    check(occurrences(emitHlsl(shader.builder.graph()), "float fn_wave(") == 1);
};

auto tDerivatives = test("FunctionCodegen/derivativesFollowTheCallingStage") = []
{
    auto fragmentOnly = DerivativeShader {false};
    auto both = DerivativeShader {true};

    check(!vertexUsesImplicitDerivatives(fragmentOnly.builder.graph()));
    check(vertexUsesImplicitDerivatives(both.builder.graph()));

    auto sources = Sources {fragmentOnly.builder.graph()};
    checkAll(sources.metal, {"float fn_edge(float v0)", "fwidth(v0)"});
    expectGlslCompiles(sources.glsl);
};

auto tTexture =
    test("FunctionCodegen/aFunctionWritesATextureAndReadsAnOverride") = []
{
    auto kernel = TextureKernel {};
    const auto& graph = kernel.builder.graph();
    auto sources = Sources {graph};

    check(graph.function(0).writesStorage && graph.isCalledAsStatement(0));
    checkAll(sources.metal,
             {"void fn_paint(uint v0, uint v1, float v2, texture2d<float, "
              "access::write> texture0)",
              "(v2 * override_gain)",
              "    fn_paint(gid.x, gid.y, float(gid.x), texture0);"});
    checkAll(sources.hlsl, {"void fn_paint(uint v0, uint v1, float v2)\n"});
    checkAll(sources.glsl, {"void fn_paint(uint v0, uint v1, float v2)\n"});
    expectGlslCompiles(sources.glsl, true);
};

auto tDiscard = test("FunctionCodegen/aFragmentFunctionDiscards") = []
{
    auto shader = DiscardShader {};
    const auto& graph = shader.builder.graph();
    auto sources = Sources {graph};

    check(graph.function(0).discards && graph.isCalledAsStatement(0));
    checkAll(sources.metal,
             {"void fn_dropTopRow(float v0)",
              "discard_fragment();",
              "    fn_dropTopRow((input.position).y);"});
    checkAll(sources.hlsl, {"void fn_dropTopRow(float v0)", "        discard;"});
    checkAll(sources.glsl,
             {"void fn_dropTopRow(float v0)",
              "        discard;",
              "    fn_dropTopRow((gl_FragCoord).y);"});
    expectGlslCompiles(sources.glsl);
};

auto tVertex = test("FunctionCodegen/aStatementCallLandsInTheStageOpen") = []
{
    auto shader = VertexStatementShader {};
    const auto& graph = shader.builder.graph();
    auto sources = Sources {graph};
    auto fragment = sources.metal.find("fragment float4 fragmentMain");

    check(graph.isVertexVariable(1), "the vertex call's result is the vertex's");
    check(!graph.isVertexVariable(2));
    check(has(sources.metal.substr(0, fragment),
              "    Particle v1 = fn_lift(input.a0);"),
          sources.metal);
    check(
        has(sources.metal.substr(fragment), "    Particle v2 = fn_lift(input.v0);"),
        sources.metal);
    check(occurrences(sources.glsl, "Particle fn_lift(vec2 v0)") == 2, sources.glsl);
    expectGlslCompiles(sources.glsl);
};
} // namespace
} // namespace FunctionCodegenTests

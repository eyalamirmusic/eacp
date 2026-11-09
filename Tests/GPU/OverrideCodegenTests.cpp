#include "CodegenCommon.h"

#include <eacp/GPU/Codegen/ShaderOverrideEmission.h>

#include <bit>
#include <climits>
#include <cmath>
#include <limits>
#include <string>
#include <string_view>

// WGSL's pipeline-overridable constants: overrideConstant<T>(name, default)
// declared as a Metal function constant, a Vulkan specialization constant and
// an HLSL macro under #ifndef, read as override_<name> in every stage; and the
// values a pipeline gives them resolved by name, checked against each
// declaration, and reduced to the ones that differ from their defaults. The
// GLSL is compiled wherever eacp-spirv is built; the values reaching a
// pipeline are checked on a device by OverrideDevice/ and on the CPU by
// CpuComputeTests' Override/.

using namespace nano;
using namespace eacp;
using namespace eacp::GPU;
using namespace eacp::GPU::ShaderEmission;

namespace OverrideCodegenTests
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

// One override of each integer and boolean type and a Float, one with no
// default, all read in one expression.
struct KernelGraph
{
    KernelGraph()
    {
        auto input = builder.inputBuffer();
        auto output = builder.outputBuffer();
        auto id = builder.threadId();

        auto gain = builder.overrideConstant<Float>("gain", 2.f);
        auto offset = builder.overrideConstant<Int>("offset", -3);
        auto taps = builder.overrideConstant<UInt>("taps");
        auto negate = builder.overrideConstant<Bool>("negate", false);

        auto value = input[id] * gain + toFloat(offset) + toFloat(taps);
        builder.write(output, id, select(negate, -value, value));
    }

    ShaderBuilder builder;
};

constexpr auto kernelMetal = std::string_view {R"golden(#include <metal_stdlib>
using namespace metal;

constant float eacpOverride0 [[function_constant(0)]];
constant float override_gain = is_function_constant_defined(eacpOverride0) ? eacpOverride0 : 2.0;
constant int eacpOverride1 [[function_constant(1)]];
constant int override_offset = is_function_constant_defined(eacpOverride1) ? eacpOverride1 : -3;
constant uint override_taps [[function_constant(2)]];
constant bool eacpOverride3 [[function_constant(3)]];
constant bool override_negate = is_function_constant_defined(eacpOverride3) ? eacpOverride3 : false;

struct Uniforms
{
    uint count;
};

kernel void computeMain(device const float* buffer0 [[buffer(0)]],
    device float* buffer1 [[buffer(1)]],
    constant Uniforms& uniforms [[buffer(16)]],
    uint gid [[thread_position_in_grid]])
{
    if (gid >= uniforms.count)
        return;
    float t0 = (((buffer0[gid] * override_gain) + float(override_offset)) + float(override_taps));
    buffer1[gid] = (override_negate ? (-(t0)) : t0);
}
)golden"};

constexpr auto kernelHlsl = std::string_view {R"golden(#ifndef EACP_OVERRIDE_0
#define EACP_OVERRIDE_0 2.0
#endif
static const float override_gain = EACP_OVERRIDE_0;
#ifndef EACP_OVERRIDE_1
#define EACP_OVERRIDE_1 -3
#endif
static const int override_offset = EACP_OVERRIDE_1;
#ifndef EACP_OVERRIDE_2
#define EACP_OVERRIDE_2 0u
#endif
static const uint override_taps = EACP_OVERRIDE_2;
#ifndef EACP_OVERRIDE_3
#define EACP_OVERRIDE_3 false
#endif
static const bool override_negate = EACP_OVERRIDE_3;

struct Uniforms
{
    uint count;
};

cbuffer UniformsCB : register(b0)
{
    Uniforms uniforms;
};

StructuredBuffer<float> buffer0 : register(t0);
RWStructuredBuffer<float> buffer1 : register(u1);

[numthreads(64, 1, 1)]
void computeMain(uint3 threadId : SV_DispatchThreadID)
{
    uint gid = threadId.x;
    if (gid >= uniforms.count)
        return;
    float t0 = (((buffer0[gid] * override_gain) + float(override_offset)) + float(override_taps));
    buffer1[gid] = (override_negate ? (-(t0)) : t0);
}
)golden"};

constexpr auto kernelGlsl = std::string_view {R"golden(#version 450

layout(constant_id = 0) const float override_gain = 2.0;
layout(constant_id = 1) const int override_offset = -3;
layout(constant_id = 2) const uint override_taps = 0u;
layout(constant_id = 3) const bool override_negate = false;

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

void main()
{
    uint gid = gl_GlobalInvocationID.x;
    if (gid >= uniforms.count)
        return;
    float t0 = (((buffer0[gid] * override_gain) + float(override_offset)) + float(override_taps));
    buffer1[gid] = (override_negate ? (-(t0)) : t0);
}
)golden"};

// An override read in the vertex stage and another in the fragment stage.
struct RenderGraph
{
    RenderGraph()
    {
        auto position = builder.vertexInput<Float2>();
        auto scale = builder.overrideConstant<Float>("scale", 0.5f);
        auto tint = builder.overrideConstant<Float>("tint");

        builder.position(float4(position * scale, 0.f, 1.f));
        builder.fragment(float4(tint, 1.f - tint, 0.f, 1.f));
    }

    ShaderBuilder builder;
};

struct HalfGraph
{
    HalfGraph()
    {
        auto output = builder.halfOutputBuffer();
        auto id = builder.threadId();
        auto step = builder.overrideConstant<Half>("step", 0.1f);

        builder.write(output, id, step * toHalf(toFloat(id)));
    }

    ShaderBuilder builder;
};

struct PlainGraph
{
    PlainGraph()
    {
        auto input = builder.inputBuffer();
        auto output = builder.outputBuffer();
        auto id = builder.threadId();
        builder.write(output, id, input[id] * 2.f);
    }

    ShaderBuilder builder;
};

PipelineConstant constantAt(const PipelineConstants& constants, int id)
{
    for (const auto& constant: constants.values)
        if (constant.id == id)
            return constant;

    return {};
}
} // namespace

auto tGoldens = test("OverrideCodegen/aKernelMatchesTheGoldens") = []
{
    auto graph = KernelGraph {};
    auto sources = Sources {graph.builder.graph()};

    check(sources.metal == kernelMetal, sources.metal);
    check(sources.hlsl == kernelHlsl, sources.hlsl);
    check(sources.glsl == kernelGlsl, sources.glsl);
    expectGlslCompiles(sources.glsl, true);
};

auto tRender = test("OverrideCodegen/bothStagesReadTheirConstants") = []
{
    auto graph = RenderGraph {};
    auto sources = Sources {graph.builder.graph()};

    checkAll(sources.metal,
             {"constant float eacpOverride0 [[function_constant(0)]];\n"
              "constant float override_scale = "
              "is_function_constant_defined(eacpOverride0) ? eacpOverride0 : 0.5;\n"
              "constant float override_tint [[function_constant(1)]];\n",
              "(input.a0 * override_scale)",
              "float4(override_tint, (1.0 - override_tint), 0.0, 1.0)"});
    checkAll(sources.hlsl,
             {"#ifndef EACP_OVERRIDE_1\n#define EACP_OVERRIDE_1 0.0\n#endif\n"
              "static const float override_tint = EACP_OVERRIDE_1;\n",
              "(input.a0 * override_scale)",
              "float4(override_tint, (1.0 - override_tint), 0.0, 1.0)"});
    checkAll(sources.glsl,
             {"layout(constant_id = 0) const float override_scale = 0.5;\n"
              "layout(constant_id = 1) const float override_tint = 0.0;\n",
              "(attr0 * override_scale)",
              "vec4(override_tint, (1.0 - override_tint), 0.0, 1.0)"});
    check(occurrences(sources.glsl, "constant_id") == 2,
          "declared once, above both stages");
    expectGlslCompiles(sources.glsl);
};

auto tHalf = test("OverrideCodegen/aHalfTakesTheF16Spellings") = []
{
    auto graph = HalfGraph {};
    auto sources = Sources {graph.builder.graph()};
    const auto& declared = graph.builder.graph().overrides()[0];

    check(declared.type == ValueType::Half);
    check(std::bit_cast<float>(declared.defaultBits) == 0.0999755859375f,
          "the default is held rounded to the nearest half");

    checkAll(sources.metal,
             {"constant half eacpOverride0 [[function_constant(0)]];\n"
              "constant half override_step = is_function_constant_defined("
              "eacpOverride0) ? eacpOverride0 : 0.099975586h;"});
    checkAll(sources.hlsl,
             {"#define EACP_OVERRIDE_0 0.099975586\n",
              "static const float override_step = EACP_OVERRIDE_0;"});
    checkAll(sources.glsl,
             {"#extension GL_EXT_shader_explicit_arithmetic_types_float16 : require",
              "layout(constant_id = 0) const float16_t override_step = "
              "0.099975586hf;"});
    expectGlslCompiles(sources.glsl, true);
};

auto tPlain = test("OverrideCodegen/aShaderWithoutOverridesDeclaresNone") = []
{
    auto plain = PlainGraph {};
    auto kernel = KernelGraph {};

    for (auto backend:
         {ShaderBackend::Metal, ShaderBackend::DirectX, ShaderBackend::Vulkan})
    {
        const auto plainSource = buildShader(plain.builder.graph(), backend).source;
        const auto kernelSource =
            buildShader(kernel.builder.graph(), backend).source;

        check(!plainSource.hasPipelineConstants);
        check(!has(plainSource.source, "override_")
              && !has(plainSource.source, "function_constant")
              && !has(plainSource.source, "constant_id")
              && !has(plainSource.source, "EACP_OVERRIDE"));
        check(kernelSource.hasPipelineConstants,
              "the source says its pipelines specialise it");
    }
};

auto tTwice = test("OverrideCodegen/aNameDeclaredAgainIsTheSameConstant") = []
{
    auto builder = ShaderBuilder {};
    auto output = builder.outputBuffer();
    auto first = builder.overrideConstant<Float>("gain", 1.f);
    auto again = builder.overrideConstant<Float>("gain", 1.f);
    auto other = builder.overrideConstant<UInt>("count");

    builder.write(output, builder.threadId(), first + again + toFloat(other));

    const auto& graph = builder.graph();
    check(first.node == again.node);
    check(graph.overrides().size() == 2);
    check(graph.findOverride("gain") == 0 && graph.findOverride("count") == 1);
    check(graph.findOverride("missing") == -1);
    check(graph.expr(other.node).kind == ExprKind::Override
          && graph.expr(other.node).index == 1);
    check(occurrences(emitMetal(graph), "[[function_constant(") == 2);
};

auto tResolve = test("OverrideCodegen/valuesResolveByNameToTheirTypes") = []
{
    auto graph = KernelGraph {};
    const auto& shader = graph.builder.graph();

    auto resolved = resolveOverrides(
        shader,
        OverrideValues {}.set("taps", 4).set("gain", 0.5).set("negate", true));

    check(resolved.isValid(), resolved.error);
    check(resolved.values.size() == 4, "every override, by id");
    check(resolved.values[0].type == PipelineConstantType::Float
          && resolved.values[0].asFloat() == 0.5f);
    check(resolved.values[1].type == PipelineConstantType::Int
              && resolved.values[1].asInt() == -3,
          "an override left out takes its default");
    check(resolved.values[2].type == PipelineConstantType::UInt
              && resolved.values[2].asUInt() == 4u,
          "a non-negative int sets a UInt");
    check(resolved.values[3].type == PipelineConstantType::Bool
          && resolved.values[3].asBool());

    check(resolved.specialized.values.size() == 3,
          "only the values that are not their default reach a pipeline");
    check(constantAt(resolved.specialized, 0).asFloat() == 0.5f);
    check(constantAt(resolved.specialized, 2).asUInt() == 4u);
    check(constantAt(resolved.specialized, 3).asBool());

    auto signedFromUnsigned = resolveOverrides(
        shader, OverrideValues {}.set("taps", 1u).set("offset", 7u));
    check(signedFromUnsigned.isValid(), signedFromUnsigned.error);
    check(signedFromUnsigned.values[1].asInt() == 7);

    auto later = OverrideValues {}.set("taps", 1u).set("taps", 9u);
    check(later.entries().size() == 1);
    check(resolveOverrides(shader, later).values[2].asUInt() == 9u,
          "setting a name again replaces its value");
};

auto tDefaults = test("OverrideCodegen/aDefaultGivenOrLeftOutIsOneSet") = []
{
    auto graph = KernelGraph {};
    const auto& shader = graph.builder.graph();

    const auto leftOut = resolveOverrides(shader, OverrideValues {}.set("taps", 2u));
    const auto given = resolveOverrides(
        shader,
        OverrideValues {}.set("taps", 2u).set("gain", 2.f).set("offset", -3));
    const auto other =
        resolveOverrides(shader, OverrideValues {}.set("taps", 2u).set("gain", 3.f));

    check(leftOut.specialized == given.specialized);
    check(leftOut.specialized.values.size() == 1,
          "an override with no default is always given");
    check(!(leftOut.specialized == other.specialized));

    auto plain = PlainGraph {};
    const auto none = resolveOverrides(plain.builder.graph(), OverrideValues {});
    check(none.isValid() && none.values.empty() && none.specialized.isEmpty());
};

auto tRefused = test("OverrideCodegen/valuesTheShaderRefusesAreNamed") = []
{
    auto graph = KernelGraph {};
    auto half = HalfGraph {};
    const auto& shader = graph.builder.graph();

    auto refusal = [&shader](const OverrideValues& values) -> std::string
    {
        const auto resolved = resolveOverrides(shader, values);
        check(!resolved.isValid());
        return resolved.error;
    };

    const auto withTaps = [] { return OverrideValues {}.set("taps", 1u); };

    check(has(refusal(OverrideValues {}), "\"taps\" (UInt): has no default"));
    check(has(refusal(withTaps().set("gian", 1.f)), "no override named \"gian\""));
    check(has(refusal(withTaps().set("gain", 1)), "\"gain\" (Float): given an int"));
    check(has(refusal(withTaps().set("negate", 1)), "\"negate\" (Bool)"));
    check(has(refusal(withTaps().set("offset", 1.f)), "\"offset\" (Int)"));
    check(has(refusal(OverrideValues {}.set("taps", -1)), "negative"));
    check(has(refusal(withTaps().set("offset", 3000000000u)), "INT_MAX"));
    check(
        has(refusal(withTaps().set("gain", std::numeric_limits<float>::infinity())),
            "not finite"));
    check(has(refusal(withTaps().set("gain", std::nanf(""))), "not finite"));

    const auto pastHalf = resolveOverrides(half.builder.graph(),
                                           OverrideValues {}.set("step", 70000.f));
    check(!pastHalf.isValid() && has(pastHalf.error, "f16 range"), pastHalf.error);

    const auto rounded =
        resolveOverrides(half.builder.graph(), OverrideValues {}.set("step", 0.3f));
    check(rounded.isValid());
    check(rounded.values[0].asFloat() == 0.300048828125f,
          "a Half's value is rounded to the nearest half");
    check(rounded.values[0].asHalfBits() == 0x34cdu);
};

auto tDefines = test("OverrideCodegen/hlslDefinesSpellEachValue") = []
{
    auto constants = PipelineConstants {};
    constants.set(PipelineConstant::ofUInt(4, 7u))
        .set(PipelineConstant::ofFloat(0, 0.25f))
        .set(PipelineConstant::ofInt(2, INT_MIN))
        .set(PipelineConstant::ofBool(1, true))
        .set(PipelineConstant::ofHalf(3, 0.1f))
        .set(PipelineConstant::ofFloat(0, 1e30f));

    check(constants.values.size() == 5, "a second value for an id replaces it");

    const auto defines = hlslDefinesFor(constants);
    check(defines.size() == 5);
    check(defines[0].name == "EACP_OVERRIDE_0" && defines[0].value == "1e+30");
    check(defines[1].name == "EACP_OVERRIDE_1" && defines[1].value == "true");
    check(defines[2].value == "(-2147483647 - 1)");
    check(defines[3].value == "0.099975586");
    check(defines[4].name == "EACP_OVERRIDE_4" && defines[4].value == "7u");

    check(printOverrideLiteral(Backend::Metal, PipelineConstant::ofHalf(0, 0.5f))
          == "0.5h");
    check(printOverrideLiteral(Backend::Vulkan, PipelineConstant::ofHalf(0, 0.5f))
          == "0.5hf");
    check(printOverrideLiteral(Backend::DirectX, PipelineConstant::ofHalf(0, 0.5f))
          == "0.5");
};

auto tSmallest = test("OverrideCodegen/theSmallestIntDefaultCompiles") = []
{
    auto builder = ShaderBuilder {};
    auto output = builder.intOutputBuffer();
    auto least = builder.overrideConstant<Int>("least", INT_MIN);
    builder.write(output, builder.threadId(), least + 1);

    const auto sources = Sources {builder.graph()};

    checkAll(sources.metal, {": (-2147483647 - 1);"});
    checkAll(sources.hlsl, {"#define EACP_OVERRIDE_0 (-2147483647 - 1)"});
    checkAll(sources.glsl, {"const int override_least = (-2147483647 - 1);"});
    expectGlslCompiles(sources.glsl, true);
};

auto tCpu = test("OverrideCodegen/theCpuExecutorPlansEveryGraph/runs") = []
{
    auto kernel = KernelGraph {};
    auto half = HalfGraph {};
    auto options = CpuCompute::PlanOptions {};
    options.overrides.set("taps", 3u);

    expectPlans(CpuCompute::Executor {kernel.builder.graph(), options});
    expectPlans(CpuCompute::Executor {half.builder.graph()});

    const auto refused = CpuCompute::Executor {kernel.builder.graph()};
    check(!refused.isValid() && has(refused.reason(), "\"taps\""), refused.reason());
};
} // namespace OverrideCodegenTests

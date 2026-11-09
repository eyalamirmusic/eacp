#include "CodegenCommon.h"

#include <initializer_list>
#include <string>

// The WGSL stage built-ins the EDSL gained beside fragmentCoordinate, vertexId
// and instanceId: front_facing, sample_index and sample_mask in, frag_depth and
// sample_mask out, and the kernel's local_invocation_index, num_workgroups and
// SIMD lane and width. Each is checked as text in all three dialects, and the
// GLSL is compiled wherever eacp-spirv is built. What the numbers are is pinned
// on a device by FragmentBuiltInTests and KernelBuiltInDeviceTests, and on the
// CPU by CpuComputeTests' KernelBuiltInTests.

using namespace nano;
using namespace eacp;
using namespace eacp::GPU;

namespace StageBuiltInCodegenTests
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

// A full-screen pass: a position straight from the one attribute.
struct RenderPass2D
{
    RenderPass2D()
    {
        auto position = builder.vertexInput<Float2>();
        builder.position(float4(position, 0.f, 1.f));
    }

    ShaderBuilder builder;
};

struct Kernel
{
    explicit Kernel(ThreadGroupShape shape = {})
    {
        output = builder.uintOutputBuffer();
        builder.setThreadGroupShape(shape);
    }

    ShaderBuilder builder;
    UIntOutputBuffer output;
};
} // namespace

auto tFragmentInputs =
    test("StageBuiltInCodegen/fragmentInputsAreEntryPointParameters") = []
{
    auto shader = RenderPass2D {};
    auto& builder = shader.builder;

    builder.fragment(float4(toFloat(builder.frontFacing()),
                            toFloat(builder.sampleIndex()),
                            toFloat(builder.sampleMask()),
                            1.f));

    auto sources = Sources {builder.graph()};

    check(contains(sources.metal, "fragment float4 fragmentMain("));
    check(contains(sources.metal, "bool frontFacing [[front_facing]]"));
    check(contains(sources.metal, "uint sampleIndex [[sample_id]]"));
    check(contains(sources.metal, "uint sampleMaskIn [[sample_mask]]"));

    check(contains(sources.hlsl,
                   "float4 fragmentMain(VertexOut input, bool frontFacing : "
                   "SV_IsFrontFace, uint sampleIndex : SV_SampleIndex, uint "
                   "sampleMaskIn : SV_Coverage) : SV_Target"));

    check(contains(sources.glsl, "gl_FrontFacing"));
    check(contains(sources.glsl, "uint(gl_SampleID)"));
    check(contains(sources.glsl, "uint(gl_SampleMaskIn[0])"));

    for (const auto* source: {&sources.metal, &sources.hlsl, &sources.glsl})
        check(!contains(*source, "FragmentOut"));

    expectGlslCompiles(builder.graph());
};

auto tNothingAsked = test("StageBuiltInCodegen/aPlainShaderDeclaresNoneOfThem") = []
{
    auto shader = RenderPass2D {};
    shader.builder.fragment(float4(shader.builder.constant(1.f), 0.f, 0.f, 1.f));

    auto sources = Sources {shader.builder.graph()};

    check(contains(sources.metal,
                   "fragment float4 fragmentMain(VertexOut input [[stage_in]])"));
    check(contains(sources.hlsl,
                   "float4 fragmentMain(VertexOut input) : SV_Target\n{\n"));
    check(contains(sources.hlsl, "    return float4("));

    for (const auto* source: {&sources.metal, &sources.hlsl, &sources.glsl})
    {
        check(!contains(*source, "FragmentOut"));
        check(!contains(*source, "frontFacing"));
        check(!contains(*source, "sampleIndex"));
        check(!contains(*source, "gl_FragDepth"));
        check(!contains(*source, "gl_SampleMask"));
    }
};

// Metal and HLSL return a struct once there is a second output; GLSL writes
// its own variables beside fragColor.
auto tDepthOutput = test("StageBuiltInCodegen/fragmentDepthIsAStructMember") = []
{
    auto shader = RenderPass2D {};
    auto& builder = shader.builder;
    auto at = builder.fragmentCoordinate();

    builder.fragment(float4(builder.constant(1.f), 0.f, 0.f, 1.f));
    builder.fragmentDepth(at.x() * 0.125f);

    auto sources = Sources {builder.graph()};

    check(contains(sources.metal, "struct FragmentOut\n{\n"));
    check(contains(sources.metal, "    float4 color [[color(0)]];\n"));
    check(contains(sources.metal, "    float depth [[depth(any)]];\n"));
    check(contains(sources.metal, "fragment FragmentOut fragmentMain("));
    check(contains(sources.metal, "    FragmentOut output;\n"));
    check(contains(sources.metal, "    output.depth = "));
    check(contains(sources.metal, "    return output;\n"));
    check(!contains(sources.metal, "sample_mask"));

    check(contains(sources.hlsl, "    float4 color : SV_Target;\n"));
    check(contains(sources.hlsl, "    float depth : SV_Depth;\n"));
    check(contains(sources.hlsl, "FragmentOut fragmentMain(VertexOut input)\n{\n"));
    check(contains(sources.hlsl, "    output.color = "));
    check(contains(sources.hlsl, "    output.depth = "));

    check(contains(sources.glsl, "    fragColor = "));
    check(contains(sources.glsl, "    gl_FragDepth = "));
    check(!contains(sources.glsl, "FragmentOut"));

    expectGlslCompiles(builder.graph());
};

auto tMaskOutput = test("StageBuiltInCodegen/sampleMaskOutputBesideItsInput") = []
{
    auto shader = RenderPass2D {};
    auto& builder = shader.builder;

    builder.fragment(float4(builder.constant(1.f), 1.f, 1.f, 1.f));
    builder.fragmentSampleMask(builder.sampleMask() & 3u);
    builder.fragmentDepth(builder.fragmentCoordinate().z());

    auto sources = Sources {builder.graph()};

    check(contains(sources.metal, "    uint sampleMask [[sample_mask]];\n"));
    check(contains(sources.metal, "uint sampleMaskIn [[sample_mask]]"));
    check(contains(sources.metal, "    output.sampleMask = "));
    check(contains(sources.metal, "    float depth [[depth(any)]];\n"));

    check(contains(sources.hlsl, "    uint sampleMask : SV_Coverage;\n"));
    check(contains(sources.hlsl,
                   "FragmentOut fragmentMain(VertexOut input, uint sampleMaskIn "
                   ": SV_Coverage)\n{\n"));
    check(contains(sources.hlsl, "    output.sampleMask = "));

    check(contains(sources.glsl, "    gl_SampleMask[0] = int("));
    check(contains(sources.glsl, "uint(gl_SampleMaskIn[0])"));
    check(contains(sources.glsl, "    gl_FragDepth = "));

    expectGlslCompiles(builder.graph());
};

// A value both outputs read is one local, planned with the colour.
auto tSharedOutputs = test("StageBuiltInCodegen/outputsShareTheirLocals") = []
{
    auto shader = RenderPass2D {};
    auto& builder = shader.builder;
    auto shared = sqrt(builder.fragmentCoordinate().x() * 3.f);

    builder.fragment(float4(shared, 0.f, 0.f, 1.f));
    builder.fragmentDepth(shared * 0.5f);

    auto sources = Sources {builder.graph()};

    for (const auto* source: {&sources.metal, &sources.hlsl, &sources.glsl})
        check(occurrences(*source, "sqrt(") == 1);

    expectGlslCompiles(builder.graph());
};

auto tLocalIndex = test("StageBuiltInCodegen/localIndexIsTheFlatLocalIndex") = []
{
    auto kernel = Kernel {{8, 4}};
    auto id = kernel.builder.threadId2();

    kernel.builder.write(
        kernel.output, id.y() * 64u + id.x(), kernel.builder.localIndex());

    auto sources = Sources {kernel.builder.graph()};

    check(
        contains(sources.metal, "uint localIndex [[thread_index_in_threadgroup]]"));
    check(contains(sources.hlsl, "uint groupLane : SV_GroupIndex"));
    check(contains(sources.glsl, "gl_LocalInvocationIndex"));

    expectGlslCompiles(kernel.builder.graph());
};

auto tGroupCount1D = test("StageBuiltInCodegen/groupCountOverAFlatGrid") = []
{
    auto kernel = Kernel {};
    auto id = kernel.builder.threadId();
    auto groups = kernel.builder.groupCount();

    kernel.builder.write(kernel.output, id, groups.x() + groups.y() + groups.z());

    auto sources = Sources {kernel.builder.graph()};

    // Metal takes every kernel input at one width, so it is gid's here.
    check(contains(sources.metal, "uint gid [[thread_position_in_grid]]"));
    check(contains(sources.metal, "uint groupCount [[threadgroups_per_grid]]"));
    check(contains(sources.metal, "uint3(groupCount, 1u, 1u)"));
    check(contains(sources.glsl, "gl_NumWorkGroups"));
    check(
        contains(sources.hlsl, "uint3(((uniforms.count - 1u) / 64u + 1u), 1u, 1u)"));
    check(!contains(sources.hlsl, "SV_GroupIndex"));

    expectGlslCompiles(kernel.builder.graph());
};

auto tGroupCount3D = test("StageBuiltInCodegen/groupCountOverAVolume") = []
{
    auto kernel = Kernel {{4, 2, 8}};
    auto id = kernel.builder.threadId3();
    auto groups = kernel.builder.groupCount();

    kernel.builder.write(
        kernel.output, id.x(), groups.x() * groups.y() * groups.z());

    auto sources = Sources {kernel.builder.graph()};

    check(contains(sources.metal, "uint3 groupCount [[threadgroups_per_grid]]"));
    check(contains(sources.hlsl,
                   "uint3(((uniforms.width - 1u) / 4u + 1u), ((uniforms.height - "
                   "1u) / 2u + 1u), ((uniforms.depth - 1u) / 8u + 1u))"));

    expectGlslCompiles(kernel.builder.graph());
};

// The emulated backends' SIMD group is simdGroupWidth consecutive flat
// indices; Metal reads its own. Neither asks for the whole-SIMD-group shape a
// SIMD-group matrix needs, so a 48-thread group is fine.
auto tSimdLane = test("StageBuiltInCodegen/simdLaneAndWidth") = []
{
    auto kernel = Kernel {{48}};
    auto id = kernel.builder.threadId();

    kernel.builder.write(kernel.output,
                         id,
                         kernel.builder.simdLaneIndex() * 1000u
                             + kernel.builder.simdGroupSize());

    auto sources = Sources {kernel.builder.graph()};

    check(contains(sources.metal, "uint simdLane [[thread_index_in_simdgroup]]"));
    check(contains(sources.metal, "uint simdSize [[threads_per_simdgroup]]"));
    check(!contains(sources.metal, "simdgroup_index_in_threadgroup"));

    check(contains(sources.hlsl, "uint groupLane : SV_GroupIndex"));
    check(contains(sources.hlsl, "(groupLane % 32u)"));
    check(contains(sources.glsl, "(gl_LocalInvocationIndex % 32u)"));

    for (const auto* source: {&sources.hlsl, &sources.glsl})
    {
        check(!contains(*source, "Wave"));
        check(!contains(*source, "gl_Subgroup"));
    }

    expectGlslCompiles(kernel.builder.graph());
};

auto tKernelUnchanged =
    test("StageBuiltInCodegen/aPlainKernelDeclaresNoneOfThem") = []
{
    auto kernel = Kernel {};
    auto id = kernel.builder.threadId();

    kernel.builder.write(kernel.output, id, id);

    auto sources = Sources {kernel.builder.graph()};

    for (const auto* source: {&sources.metal, &sources.hlsl, &sources.glsl})
    {
        check(!contains(*source, "localIndex"));
        check(!contains(*source, "groupCount"));
        check(!contains(*source, "simdLane"));
        check(!contains(*source, "simdSize"));
        check(!contains(*source, "groupLane"));
        check(!contains(*source, "gl_NumWorkGroups"));
    }
};

// The CPU executor runs the kernel built-ins and refuses a render one.
auto tCpuPlans = test("StageBuiltInCodegen/theKernelBuiltInsPlanOnTheCpu") = []
{
    auto kernel = Kernel {};
    auto id = kernel.builder.threadId();
    auto groups = kernel.builder.groupCount();

    kernel.builder.write(kernel.output,
                         id,
                         kernel.builder.localIndex() + groups.x()
                             + kernel.builder.simdLaneIndex()
                             + kernel.builder.simdGroupSize());

    auto executor = CpuCompute::Executor {kernel.builder.graph()};
    check(executor.isValid(), executor.reason());

    auto render = Kernel {};
    auto renderId = render.builder.threadId();
    render.builder.write(
        render.output, renderId, select(render.builder.frontFacing(), 1u, 0u));

    auto refused = CpuCompute::Executor {render.builder.graph()};
    check(!refused.isValid());
    check(contains(refused.reason(), "render graph input"));
};
} // namespace StageBuiltInCodegenTests

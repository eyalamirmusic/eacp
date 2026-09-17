#include "CodegenCommon.h"

// One group-wide fold, three dialects: a SIMD-group intrinsic on Metal and a
// groupshared tree where there is no wave intrinsic to reach for.

using namespace nano;
using namespace eacp;
using namespace eacp::GPU;

namespace
{
auto has(const std::string& source, std::string_view text)
{
    return source.find(text) != std::string::npos;
}

constexpr auto groupSize = ComputePass::threadGroupWidth;
} // namespace

auto tGroupReductionSource = test("GroupReduction/eachBackendFoldsItsOwnWay") = []
{
    auto builder = ShaderBuilder {};

    auto input = builder.inputBuffer();
    auto output = builder.outputBuffer();
    auto id = builder.threadId();
    auto value = input[id];

    auto total = builder.groupSum(value);
    auto peak = builder.groupMax(value);
    auto least = builder.groupMin(value);

    builder.write(output, id, total + peak + least);

    const auto& graph = builder.graph();
    auto metal = emitMetal(graph);
    auto hlsl = emitHlsl(graph);
    auto glsl = emitGlsl(graph);

    auto width = std::to_string(groupSize);

    // Metal reduces within each SIMD group and combines the partials, so it
    // needs the lane, the SIMD group's index and how many there are.
    check(has(metal, "uint simdLane [[thread_index_in_simdgroup]]"));
    check(has(metal, "uint simdIndex [[simdgroup_index_in_threadgroup]]"));
    check(has(metal, "uint simdCount [[simdgroups_per_threadgroup]]"));
    check(has(metal, "threadgroup float groupScratch[" + width + "];"));
    check(has(metal, "float v0 = simd_sum("));
    check(has(metal, "float v1 = simd_max("));
    check(has(metal, "float v2 = simd_min("));
    check(has(metal, "if (simdLane == 0u)\n        groupScratch[simdIndex] = v0;"));
    check(has(metal, "for (uint gr0 = 1u; gr0 < simdCount; ++gr0)"));
    check(has(metal, "v1 = max(v1, groupScratch[gr1]);"));
    check(has(metal, "v2 = min(v2, groupScratch[gr2]);"));

    // FXC at cs_5_0 has no wave intrinsic, so the whole fold is the tree.
    check(has(hlsl, "groupshared float groupScratch[" + width + "];"));
    check(has(hlsl, "uint groupLane : SV_GroupIndex"));
    check(has(hlsl, "groupScratch[groupLane] = "));
    check(has(hlsl, "for (uint gr0 = 32u; gr0 > 0u; gr0 >>= 1u)"));
    check(
        has(hlsl,
            "if (groupLane < gr0 && groupLane + gr0 < " + width
                + "u)\n"
                  "            groupScratch[groupLane] = groupScratch[groupLane] + "
                  "groupScratch[groupLane + gr0];"));
    check(has(hlsl, "GroupMemoryBarrierWithGroupSync();"));
    check(has(hlsl, "float v0 = groupScratch[0];"));
    check(!has(hlsl, "WaveActiveSum"));

    // GLSL takes the same tree, indexed by the builtin flat local id, and asks
    // for no subgroup extension.
    check(has(glsl, "shared float groupScratch[" + width + "];"));
    check(has(glsl, "groupScratch[gl_LocalInvocationIndex] = "));
    check(has(glsl, "memoryBarrierShared();"));
    check(!has(glsl, "GL_KHR_shader_subgroup"));

    expectGlslCompiles(graph);
};

// The SIMD-scoped fold: one instruction on Metal, and the same scratch tree
// narrowed to a thread's own block of lanes where there is no wave intrinsic
// to reach for.
auto tSimdReductionSource =
    test("GroupReduction/aSimdFoldIsOneInstructionOnMetal") = []
{
    auto builder = ShaderBuilder {};

    auto input = builder.inputBuffer();
    auto output = builder.outputBuffer();
    auto id = builder.threadId();
    auto value = input[id];

    auto total = builder.simdSum(value);
    auto peak = builder.simdMax(value);
    auto least = builder.simdMin(value);

    builder.write(output, id, total + peak + least);

    const auto& graph = builder.graph();
    auto metal = emitMetal(graph);
    auto hlsl = emitHlsl(graph);
    auto glsl = emitGlsl(graph);

    auto width = std::to_string(simdGroupWidth);

    // The whole of it on Metal - no scratch array, no barrier, and none of the
    // three SIMD-group builtins the wide fold's combine walks.
    check(has(metal, "float v0 = simd_sum("));
    check(has(metal, "float v1 = simd_max("));
    check(has(metal, "float v2 = simd_min("));
    check(!has(metal, "groupScratch"));
    check(!has(metal, "threadgroup_barrier"));
    check(!has(metal, "simdgroups_per_threadgroup"));
    check(!has(metal, "thread_index_in_simdgroup"));

    // The tree elsewhere, over the lanes of the folding thread's own SIMD group
    // rather than over the whole group, and read back from that block's first
    // slot rather than from slot zero.
    check(has(hlsl,
              "groupshared float groupScratch[" + std::to_string(groupSize) + "];"));
    check(has(hlsl, "for (uint gr0 = 16u; gr0 > 0u; gr0 >>= 1u)"));
    check(has(hlsl,
              "if ((groupLane % " + width + "u) < gr0 && (groupLane % " + width
                  + "u) + gr0 < " + width + "u && groupLane + gr0 < "
                  + std::to_string(groupSize) + "u)"));
    check(has(hlsl,
              "float v0 = groupScratch[(groupLane / " + width + "u) * " + width
                  + "u];"));
    check(!has(hlsl, "WaveActiveSum"));

    check(has(glsl, "for (uint gr0 = 16u; gr0 > 0u; gr0 >>= 1u)"));
    check(has(glsl,
              "if ((gl_LocalInvocationIndex % " + width
                  + "u) < gr0 && (gl_LocalInvocationIndex % " + width + "u) + gr0 < "
                  + width + "u && gl_LocalInvocationIndex + gr0 < "
                  + std::to_string(groupSize) + "u)"));
    check(has(glsl,
              "float v0 = groupScratch[(gl_LocalInvocationIndex / " + width + "u) * "
                  + width + "u];"));

    // No subgroup extension: a GLSL subgroup is whatever width the device says
    // it is, where simdWidth is thirty-two by construction everywhere else in
    // the EDSL, so the intrinsic would be a different fold under one name.
    check(!has(glsl, "GL_KHR_shader_subgroup"));
    check(!has(glsl, "subgroupAdd"));

    expectGlslCompiles(graph);
};

// A group of simdWidth threads looks like one SIMD group and is not necessarily
// one: threadExecutionWidth is a property of the compiled pipeline, and an
// Intel Mac runs a kernel at eight or sixteen lanes. So the wide fold keeps its
// combine here exactly as it does in a wider group - simdCount is whatever the
// hardware gave - and only the narrow scope, which commits to simdWidth by
// definition, is the intrinsic alone.
auto tOneSimdGroupStillCombines =
    test("GroupReduction/aGroupOfOneSimdGroupStillCombines") = []
{
    auto builder = ShaderBuilder {};

    builder.setThreadGroupShape({simdGroupWidth});

    auto input = builder.inputBuffer();
    auto output = builder.outputBuffer();
    auto id = builder.threadId();

    builder.write(output, id, builder.groupSum(input[id]));

    const auto& graph = builder.graph();
    auto metal = emitMetal(graph);

    check(has(metal, "float v0 = simd_sum("));
    check(has(metal, "threadgroup float groupScratch[32];"));
    check(has(metal, "if (simdLane == 0u)"));
    check(has(metal, "for (uint gr0 = 1u; gr0 < simdCount; ++gr0)"));
    check(has(metal, "uint simdCount [[simdgroups_per_threadgroup]]"));

    // The two backends with no wave intrinsic keep the tree, which over one
    // SIMD group's worth of threads is what it always was.
    check(has(emitHlsl(graph), "groupshared float groupScratch[32];"));
    check(has(emitHlsl(graph), "if (groupLane < gr0 && groupLane + gr0 < 32u)"));
    check(has(emitGlsl(graph), "shared float groupScratch[32];"));

    expectGlslCompiles(graph);
};

// The narrow tree indexes with the global lane and counts with the lane within
// its block, so the scratch's own bound is a third conjunct rather than
// something the second implies. A release build has no assert to stop a group
// that is not a whole number of SIMD groups, and this is what keeps such a
// kernel inside its array.
auto tNarrowTreeStaysInsideTheScratch =
    test("GroupReduction/theNarrowTreeBoundsTheScratchItself") = []
{
    auto builder = ShaderBuilder {};

    auto input = builder.inputBuffer();
    auto output = builder.outputBuffer();
    auto id = builder.threadId();

    builder.write(output, id, builder.simdSum(input[id]));

    const auto& graph = builder.graph();
    auto width = std::to_string(simdGroupWidth);
    auto threads = std::to_string(groupSize);

    check(has(emitHlsl(graph),
              "if ((groupLane % " + width + "u) < gr0 && (groupLane % " + width
                  + "u) + gr0 < " + width + "u && groupLane + gr0 < " + threads
                  + "u)"));
    check(has(emitGlsl(graph), "gl_LocalInvocationIndex + gr0 < " + threads + "u)"));

    // The wide fold indexes and counts with the same lane, so it needs no such
    // conjunct and keeps the two it always had.
    auto wide = ShaderBuilder {};
    auto source = wide.inputBuffer();
    auto target = wide.outputBuffer();
    auto index = wide.threadId();

    wide.write(target, index, wide.groupSum(source[index]));

    check(has(emitHlsl(wide.graph()),
              "if (groupLane < gr0 && groupLane + gr0 < " + threads + "u)"));

    expectGlslCompiles(graph);
    expectGlslCompiles(wide.graph());
};

// Both scopes in one kernel: Metal needs the scratch for the wide fold and
// nothing for the narrow one, so the array is there and one fold touches it.
auto tBothScopesInOneKernel =
    test("GroupReduction/theTwoScopesShareOneScratchArray") = []
{
    auto builder = ShaderBuilder {};

    auto input = builder.inputBuffer();
    auto output = builder.outputBuffer();
    auto id = builder.threadId();
    auto value = input[id];

    auto whole = builder.groupSum(value);
    auto narrow = builder.simdSum(value);

    builder.write(output, id, whole + narrow);

    const auto& graph = builder.graph();
    auto metal = emitMetal(graph);

    auto width = std::to_string(groupSize);

    check(has(metal, "threadgroup float groupScratch[" + width + "];"));
    check(has(metal, "float v0 = simd_sum("));
    check(has(metal, "float v1 = simd_sum("));

    // The wide fold combines its partials; the narrow one is the call and the
    // semicolon, so the kernel holds exactly one combine loop.
    check(has(metal, "for (uint gr0 = 1u; gr0 < simdCount; ++gr0)"));
    check(!has(metal, "gr1"));

    check(has(emitHlsl(graph), "groupshared float groupScratch[" + width + "];"));
    check(has(emitGlsl(graph), "shared float groupScratch[" + width + "];"));

    expectGlslCompiles(graph);
};

// The scratch belongs to the reductions, so a kernel with none declares none.
auto tNoReductionNoScratch =
    test("GroupReduction/aKernelWithoutOneDeclaresNone") = []
{
    auto builder = ShaderBuilder {};

    auto input = builder.inputBuffer();
    auto output = builder.outputBuffer();
    auto id = builder.threadId();

    builder.write(output, id, input[id] * 2.0f);

    const auto& graph = builder.graph();

    for (const auto& source: {emitMetal(graph), emitHlsl(graph), emitGlsl(graph)})
    {
        check(!has(source, "groupScratch"));
        check(!has(source, "simd_sum"));
        check(!has(source, "SV_GroupIndex"));
        check(!has(source, "simdgroups_per_threadgroup"));
    }

    expectGlslCompiles(graph);
};

// A reduction barriers, so the kernel loses its early-return bounds guard the
// same way an explicit barrier() takes it away.
auto tReductionDropsTheGuard = test("GroupReduction/theBoundsGuardGivesWay") = []
{
    auto builder = ShaderBuilder {};

    auto input = builder.inputBuffer();
    auto output = builder.outputBuffer();
    auto id = builder.threadId();

    builder.write(output, id, builder.groupSum(input[id]));

    const auto& graph = builder.graph();

    check(graph.usesBarrier());
    check(!has(emitMetal(graph), "if (gid >= uniforms.count)"));
    check(!has(emitHlsl(graph), "if (gid >= uniforms.count)"));
    check(!has(emitGlsl(graph), "if (gid >= uniforms.count)"));
};

// The unsigned siblings fold through a scratch array of their own, so a kernel
// reducing both types declares two.
auto tUIntReductionHasItsOwnScratch =
    test("GroupReduction/theUnsignedFoldTakesItsOwnScratch") = []
{
    auto builder = ShaderBuilder {};

    auto input = builder.uintInputBuffer();
    auto output = builder.outputBuffer();
    auto id = builder.threadId();

    auto counted = builder.groupSum(input[id]);
    auto widest = builder.groupSum(toFloat(input[id]));

    builder.write(output, id, toFloat(counted) + widest);

    const auto& graph = builder.graph();
    auto metal = emitMetal(graph);
    auto hlsl = emitHlsl(graph);
    auto glsl = emitGlsl(graph);

    auto width = std::to_string(groupSize);

    check(has(metal, "threadgroup uint groupScratchU[" + width + "];"));
    check(has(metal, "threadgroup float groupScratch[" + width + "];"));
    check(has(metal, "uint v0 = simd_sum("));
    check(has(metal, "float v1 = simd_sum("));

    check(has(hlsl, "groupshared uint groupScratchU[" + width + "];"));
    check(has(hlsl, "groupshared float groupScratch[" + width + "];"));

    check(has(glsl, "shared uint groupScratchU[" + width + "];"));
    check(has(glsl, "shared float groupScratch[" + width + "];"));

    expectGlslCompiles(graph);
};

// A 2D kernel folds over the whole 8x8 group, not over one of its rows.
auto tTwoDimensionalGroupFolds = test("GroupReduction/aTwoDGroupFoldsWhole") = []
{
    auto builder = ShaderBuilder {};

    auto input = builder.inputBuffer();
    auto output = builder.outputBuffer();
    auto position = builder.threadPosition();
    auto index = position.y * builder.gridWidth() + position.x;

    builder.write(output, index, builder.groupSum(input[index]));

    const auto& graph = builder.graph();
    auto threads = std::to_string(ComputePass::threadGroupSize2D
                                  * ComputePass::threadGroupSize2D);

    check(has(emitMetal(graph), "threadgroup float groupScratch[" + threads + "];"));
    check(has(emitHlsl(graph), "groupshared float groupScratch[" + threads + "];"));
    check(has(emitHlsl(graph), "[numthreads(8, 8, 1)]"));
    check(has(emitGlsl(graph), "shared float groupScratch[" + threads + "];"));

    expectGlslCompiles(graph);
};

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

#include "CodegenCommon.h"

#include <cmath>

// One group-wide fold, three dialects: a SIMD-group intrinsic on Metal and a
// groupshared tree where there is no wave intrinsic to reach for. Each graph
// also runs on the CPU executor, whose fold is that tree, so a C++ copy of it
// is the reference and float sums compare exactly.

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

int widthOf(ReductionScope scope)
{
    return scope == ReductionScope::Group ? (int) groupSize : simdGroupWidth;
}

Float sumIn(ShaderBuilder& builder, ReductionScope scope, const Float& value)
{
    return scope == ReductionScope::Group ? builder.groupSum(value)
                                          : builder.simdSum(value);
}

Float maxIn(ShaderBuilder& builder, ReductionScope scope, const Float& value)
{
    return scope == ReductionScope::Group ? builder.groupMax(value)
                                          : builder.simdMax(value);
}

Float minIn(ShaderBuilder& builder, ReductionScope scope, const Float& value)
{
    return scope == ReductionScope::Group ? builder.groupMin(value)
                                          : builder.simdMin(value);
}

struct ThreeFoldKernel
{
    explicit ThreeFoldKernel(ReductionScope scope)
    {
        input = builder.inputBuffer();
        output = builder.outputBuffer();
        auto id = builder.threadId();
        auto value = input[id];

        auto total = sumIn(builder, scope, value);
        auto peak = maxIn(builder, scope, value);
        auto least = minIn(builder, scope, value);

        builder.write(output, id, total + peak + least);
    }

    const ShaderGraph& graph() const { return builder.graph(); }

    ShaderBuilder builder;
    InputBuffer input;
    OutputBuffer output;
};

struct SumKernel
{
    explicit SumKernel(ReductionScope scope, int threads = (int) groupSize)
    {
        if (threads != (int) groupSize)
            builder.setThreadGroupShape({threads});

        input = builder.inputBuffer();
        output = builder.outputBuffer();
        auto id = builder.threadId();

        builder.write(output, id, sumIn(builder, scope, input[id]));
    }

    const ShaderGraph& graph() const { return builder.graph(); }

    ShaderBuilder builder;
    InputBuffer input;
    OutputBuffer output;
};

struct BothScopesKernel
{
    BothScopesKernel()
    {
        input = builder.inputBuffer();
        output = builder.outputBuffer();
        auto id = builder.threadId();
        auto value = input[id];

        auto whole = builder.groupSum(value);
        auto narrow = builder.simdSum(value);

        builder.write(output, id, whole + narrow);
    }

    const ShaderGraph& graph() const { return builder.graph(); }

    ShaderBuilder builder;
    InputBuffer input;
    OutputBuffer output;
};

struct DoublingKernel
{
    DoublingKernel()
    {
        input = builder.inputBuffer();
        output = builder.outputBuffer();
        auto id = builder.threadId();

        builder.write(output, id, input[id] * 2.0f);
    }

    const ShaderGraph& graph() const { return builder.graph(); }

    ShaderBuilder builder;
    InputBuffer input;
    OutputBuffer output;
};

struct MixedTypesKernel
{
    MixedTypesKernel()
    {
        input = builder.uintInputBuffer();
        output = builder.outputBuffer();
        auto id = builder.threadId();

        auto counted = builder.groupSum(input[id]);
        auto widest = builder.groupSum(toFloat(input[id]));

        builder.write(output, id, toFloat(counted) + widest);
    }

    const ShaderGraph& graph() const { return builder.graph(); }

    ShaderBuilder builder;
    UIntInputBuffer input;
    OutputBuffer output;
};

struct TileSumKernel
{
    TileSumKernel()
    {
        input = builder.inputBuffer();
        output = builder.outputBuffer();
        auto position = builder.threadPosition();
        auto index = position.y * builder.gridWidth() + position.x;

        builder.write(output, index, builder.groupSum(input[index]));
    }

    const ShaderGraph& graph() const { return builder.graph(); }

    ShaderBuilder builder;
    InputBuffer input;
    OutputBuffer output;
};

// The fallback's tree over one block: at each halving step, lane w takes
// fold(itself, lane w + step), and the result is lane 0.
template <typename T, typename Fold>
T halvingFold(Vector<T> lanes, Fold fold)
{
    auto width = (int) lanes.size();
    auto step = 1;

    while (step * 2 < width)
        step *= 2;

    for (; step > 0; step >>= 1)
        for (auto lane = 0; lane < step && lane + step < width; ++lane)
            lanes[lane] = fold(lanes[lane], lanes[lane + step]);

    return lanes[0];
}

// What `thread` reads back from a fold over blocks of `width` lanes, with the
// lanes past the end of the input reading zero, as the executor's do.
template <typename T, typename Fold>
T foldOfBlock(const Vector<T>& input, int thread, int width, Fold fold)
{
    auto first = thread / width * width;
    auto lanes = Vector<T> {};

    for (auto lane = first; lane < first + width; ++lane)
        lanes.add(lane < (int) input.size() ? input[lane] : T {});

    return halvingFold(lanes, fold);
}

auto addValues = [](auto a, auto b) { return a + b; };
auto maxValues = [](float a, float b) { return std::fmax(a, b); };
auto minValues = [](float a, float b) { return std::fmin(a, b); };

Vector<float> wavyInput(int count)
{
    auto values = Vector<float> {};

    for (auto i = 0; i < count; ++i)
        values.add(std::sin((float) i * 0.37f) * 3.1f + 0.01f * (float) i);

    return values;
}

template <typename Kernel, typename Input>
Vector<float>
    runOnCpu(Kernel& kernel,
             const Input& input,
             int outputCount,
             int threads,
             const std::source_location& location = std::source_location::current())
{
    auto executor = CpuCompute::Executor {kernel.graph()};
    expectPlans(executor, location);

    auto output = filledWith(outputCount, -1.f);

    auto bindings = CpuCompute::Bindings {};
    bindings.set(kernel.input, input);
    bindings.set(kernel.output, output);
    check(executor.dispatch(bindings, threads), "dispatch", location);

    return output;
}

float threeFoldsOf(const Vector<float>& input, int thread, int width)
{
    auto total = foldOfBlock(input, thread, width, addValues);
    auto peak = foldOfBlock(input, thread, width, maxValues);
    auto least = foldOfBlock(input, thread, width, minValues);

    return total + peak + least;
}

void checkThreeFoldsRun(
    ReductionScope scope,
    const std::source_location& location = std::source_location::current())
{
    constexpr auto count = 3 * (int) groupSize;

    auto kernel = ThreeFoldKernel {scope};
    auto input = wavyInput(count);
    auto output = runOnCpu(kernel, input, count, count, location);

    for (auto i = 0; i < count; ++i)
        check(output[i] == threeFoldsOf(input, i, widthOf(scope)), "fold", location);
}

void checkSumsRun(
    SumKernel& kernel,
    int width,
    int count,
    const std::source_location& location = std::source_location::current())
{
    auto input = wavyInput(count);
    auto output = runOnCpu(kernel, input, count, count, location);

    for (auto i = 0; i < count; ++i)
        check(output[i] == foldOfBlock(input, i, width, addValues), "sum", location);
}
} // namespace

auto tGroupReductionSource = test("GroupReduction/eachBackendFoldsItsOwnWay") = []
{
    auto kernel = ThreeFoldKernel {ReductionScope::Group};

    const auto& graph = kernel.graph();
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

auto tGroupReductionRuns = test("GroupReduction/eachBackendFoldsItsOwnWay/runs") = []
{ checkThreeFoldsRun(ReductionScope::Group); };

// The SIMD-scoped fold: one instruction on Metal, and the same scratch tree
// narrowed to a thread's own block of lanes where there is no wave intrinsic
// to reach for.
auto tSimdReductionSource =
    test("GroupReduction/aSimdFoldIsOneInstructionOnMetal") = []
{
    auto kernel = ThreeFoldKernel {ReductionScope::Simd};

    const auto& graph = kernel.graph();
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

auto tSimdReductionRuns =
    test("GroupReduction/aSimdFoldIsOneInstructionOnMetal/runs") = []
{ checkThreeFoldsRun(ReductionScope::Simd); };

// A group of simdWidth threads looks like one SIMD group and is not necessarily
// one: threadExecutionWidth is a property of the compiled pipeline, and an
// Intel Mac runs a kernel at eight or sixteen lanes. So the wide fold keeps its
// combine here exactly as it does in a wider group - simdCount is whatever the
// hardware gave - and only the narrow scope, which commits to simdWidth by
// definition, is the intrinsic alone.
auto tOneSimdGroupStillCombines =
    test("GroupReduction/aGroupOfOneSimdGroupStillCombines") = []
{
    auto kernel = SumKernel {ReductionScope::Group, simdGroupWidth};

    const auto& graph = kernel.graph();
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

auto tOneSimdGroupRuns =
    test("GroupReduction/aGroupOfOneSimdGroupStillCombines/runs") = []
{
    auto kernel = SumKernel {ReductionScope::Group, simdGroupWidth};
    checkSumsRun(kernel, simdGroupWidth, 4 * simdGroupWidth);
};

// The narrow tree indexes with the global lane and counts with the lane within
// its block, so the scratch's own bound is a third conjunct rather than
// something the second implies. A release build has no assert to stop a group
// that is not a whole number of SIMD groups, and this is what keeps such a
// kernel inside its array.
auto tNarrowTreeStaysInsideTheScratch =
    test("GroupReduction/theNarrowTreeBoundsTheScratchItself") = []
{
    auto kernel = SumKernel {ReductionScope::Simd};

    const auto& graph = kernel.graph();
    auto width = std::to_string(simdGroupWidth);
    auto threads = std::to_string(groupSize);

    check(has(emitHlsl(graph),
              "if ((groupLane % " + width + "u) < gr0 && (groupLane % " + width
                  + "u) + gr0 < " + width + "u && groupLane + gr0 < " + threads
                  + "u)"));
    check(has(emitGlsl(graph), "gl_LocalInvocationIndex + gr0 < " + threads + "u)"));

    // The wide fold indexes and counts with the same lane, so it needs no such
    // conjunct and keeps the two it always had.
    auto wide = SumKernel {ReductionScope::Group};

    check(has(emitHlsl(wide.graph()),
              "if (groupLane < gr0 && groupLane + gr0 < " + threads + "u)"));

    expectGlslCompiles(graph);
    expectGlslCompiles(wide.graph());
};

auto tNarrowTreeRuns =
    test("GroupReduction/theNarrowTreeBoundsTheScratchItself/runs") = []
{
    constexpr auto count = 2 * (int) groupSize;

    auto narrow = SumKernel {ReductionScope::Simd};
    checkSumsRun(narrow, simdGroupWidth, count);

    auto wide = SumKernel {ReductionScope::Group};
    checkSumsRun(wide, (int) groupSize, count);
};

// Both scopes in one kernel: Metal needs the scratch for the wide fold and
// nothing for the narrow one, so the array is there and one fold touches it.
auto tBothScopesInOneKernel =
    test("GroupReduction/theTwoScopesShareOneScratchArray") = []
{
    auto kernel = BothScopesKernel {};

    const auto& graph = kernel.graph();
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

auto tBothScopesRun =
    test("GroupReduction/theTwoScopesShareOneScratchArray/runs") = []
{
    constexpr auto count = 2 * (int) groupSize;

    auto kernel = BothScopesKernel {};
    auto input = wavyInput(count);
    auto output = runOnCpu(kernel, input, count, count);

    for (auto i = 0; i < count; ++i)
    {
        auto whole = foldOfBlock(input, i, (int) groupSize, addValues);
        auto narrow = foldOfBlock(input, i, simdGroupWidth, addValues);
        check(output[i] == whole + narrow);
    }
};

// The scratch belongs to the reductions, so a kernel with none declares none.
auto tNoReductionNoScratch =
    test("GroupReduction/aKernelWithoutOneDeclaresNone") = []
{
    auto kernel = DoublingKernel {};

    const auto& graph = kernel.graph();

    for (const auto& source: {emitMetal(graph), emitHlsl(graph), emitGlsl(graph)})
    {
        check(!has(source, "groupScratch"));
        check(!has(source, "simd_sum"));
        check(!has(source, "SV_GroupIndex"));
        check(!has(source, "simdgroups_per_threadgroup"));
    }

    expectGlslCompiles(graph);
};

// With no barrier the bounds guard stands, so the lanes of the last group past
// the extent store nothing even where the output has room.
auto tNoReductionRuns =
    test("GroupReduction/aKernelWithoutOneDeclaresNone/runs") = []
{
    constexpr auto count = 100;
    constexpr auto room = 2 * (int) groupSize;

    auto kernel = DoublingKernel {};
    auto input = wavyInput(count);
    auto output = runOnCpu(kernel, input, room, count);

    for (auto i = 0; i < count; ++i)
        check(output[i] == input[i] * 2.0f);

    for (auto i = count; i < room; ++i)
        check(output[i] == -1.f);
};

// A reduction barriers, so the kernel loses its early-return bounds guard the
// same way an explicit barrier() takes it away.
auto tReductionDropsTheGuard = test("GroupReduction/theBoundsGuardGivesWay") = []
{
    auto kernel = SumKernel {ReductionScope::Group};

    const auto& graph = kernel.graph();

    check(graph.usesBarrier());
    check(!has(emitMetal(graph), "if (gid >= uniforms.count)"));
    check(!has(emitHlsl(graph), "if (gid >= uniforms.count)"));
    check(!has(emitGlsl(graph), "if (gid >= uniforms.count)"));
};

// Without the guard every lane of the last group runs: the lanes past the
// extent read zero, fold with the rest, and store the group's sum wherever the
// output has room, and nothing lands past the last group.
auto tReductionDropsTheGuardRuns =
    test("GroupReduction/theBoundsGuardGivesWay/runs") = []
{
    constexpr auto count = 100;
    constexpr auto groups = 2 * (int) groupSize;
    constexpr auto room = groups + 16;

    auto kernel = SumKernel {ReductionScope::Group};
    auto input = wavyInput(count);
    auto output = runOnCpu(kernel, input, room, count);

    for (auto i = 0; i < groups; ++i)
        check(output[i] == foldOfBlock(input, i, (int) groupSize, addValues));

    for (auto i = groups; i < room; ++i)
        check(output[i] == -1.f);
};

// The unsigned siblings fold through a scratch array of their own, so a kernel
// reducing both types declares two.
auto tUIntReductionHasItsOwnScratch =
    test("GroupReduction/theUnsignedFoldTakesItsOwnScratch") = []
{
    auto kernel = MixedTypesKernel {};

    const auto& graph = kernel.graph();
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

// Small integers, so both sums are exact and the two folds must agree.
auto tUIntReductionRuns =
    test("GroupReduction/theUnsignedFoldTakesItsOwnScratch/runs") = []
{
    constexpr auto count = 2 * (int) groupSize;

    auto kernel = MixedTypesKernel {};
    auto input = Vector<std::uint32_t> {};

    for (auto i = 0; i < count; ++i)
        input.add((std::uint32_t) (i * 7 % 23));

    auto output = runOnCpu(kernel, input, count, count);

    for (auto i = 0; i < count; ++i)
    {
        auto first = i / (int) groupSize * (int) groupSize;
        auto sum = 0u;

        for (auto lane = first; lane < first + (int) groupSize; ++lane)
            sum += input[lane];

        check(output[i] == 2.f * (float) sum);
    }
};

// A 2D kernel folds over the whole 8x8 group, not over one of its rows.
auto tTwoDimensionalGroupFolds = test("GroupReduction/aTwoDGroupFoldsWhole") = []
{
    auto kernel = TileSumKernel {};

    const auto& graph = kernel.graph();
    auto threads = std::to_string(ComputePass::threadGroupSize2D
                                  * ComputePass::threadGroupSize2D);

    check(has(emitMetal(graph), "threadgroup float groupScratch[" + threads + "];"));
    check(has(emitHlsl(graph), "groupshared float groupScratch[" + threads + "];"));
    check(has(emitHlsl(graph), "[numthreads(8, 8, 1)]"));
    check(has(emitGlsl(graph), "shared float groupScratch[" + threads + "];"));

    expectGlslCompiles(graph);
};

// The tree runs over the flat local index, row by row of the tile, so the
// reference lays each 8x8 tile out in that order before folding it.
auto tTwoDimensionalGroupRuns = test("GroupReduction/aTwoDGroupFoldsWhole/runs") = []
{
    constexpr auto side = (int) ComputePass::threadGroupSize2D;
    constexpr auto width = 3 * side;
    constexpr auto height = 2 * side;

    auto kernel = TileSumKernel {};
    auto input = wavyInput(width * height);

    auto executor = CpuCompute::Executor {kernel.graph()};
    expectPlans(executor);

    auto output = filledWith(width * height, -1.f);

    auto bindings = CpuCompute::Bindings {};
    bindings.set(kernel.input, input);
    bindings.set(kernel.output, output);
    check(executor.dispatch(bindings, width, height));

    for (auto y = 0; y < height; ++y)
    {
        for (auto x = 0; x < width; ++x)
        {
            auto left = x / side * side;
            auto top = y / side * side;
            auto tile = Vector<float> {};

            for (auto row = 0; row < side; ++row)
                for (auto column = 0; column < side; ++column)
                    tile.add(input[(top + row) * width + left + column]);

            check(output[y * width + x] == halvingFold(tile, addValues));
        }
    }
};

#include "CodegenCommon.h"

// One 8x8 fragment, three dialects: MSL's own type and its three intrinsics on
// Metal, and a pair of elements per lane exchanged through threadgroup memory
// on the two backends with no wave matrix operation to lower to.

using namespace nano;
using namespace eacp;
using namespace eacp::GPU;

namespace
{
auto has(const std::string& source, std::string_view text)
{
    return source.find(text) != std::string::npos;
}

int count(const std::string& source, std::string_view text)
{
    auto found = 0;

    for (auto at = source.find(text); at != std::string::npos;
         at = source.find(text, at + text.size()))
        ++found;

    return found;
}

// The whole vocabulary in one kernel: a fragment filled, one loaded out of a
// threadgroup tile and one out of a buffer, the product of the two accumulated
// into the first, and the result written back both ways.
ShaderBuilder productKernel()
{
    auto builder = ShaderBuilder {};
    builder.setThreadGroupShape({128});

    auto a = builder.inputBuffer();
    auto output = builder.outputBuffer();
    auto tile = builder.shared<Float>(1024);
    auto lane = builder.localId();
    auto simd = builder.simdGroupIndex();

    builder.write(tile, lane, a[lane]);
    builder.barrier();

    auto accumulator = builder.simdMatrix();
    auto left = builder.simdMatrix(tile, simd * 64u, builder.unsignedInteger(8u));
    auto right = builder.simdMatrix(a, simd * 64u, builder.unsignedInteger(8u));

    builder.multiplyAccumulate(accumulator, left, right);

    builder.write(tile, simd * 64u, builder.unsignedInteger(8u), accumulator);
    builder.write(output, simd * 64u, builder.unsignedInteger(8u), accumulator);

    return builder;
}
} // namespace

auto tSimdMatrixSource = test("SimdMatrix/eachBackendSpellsItsOwnWay") = []
{
    auto builder = productKernel();

    const auto& graph = builder.graph();
    auto metal = emitMetal(graph);
    auto hlsl = emitHlsl(graph);
    auto glsl = emitGlsl(graph);

    // Metal has the type and the three operations, so every statement is one
    // line and the fragment never becomes anything a thread holds alone.
    check(has(metal, "uint simdIndex [[simdgroup_index_in_threadgroup]]"));
    check(has(metal,
              "simdgroup_float8x8 sgm0 = make_filled_simdgroup_matrix<float, 8, "
              "8>(0.0);"));
    check(has(metal, "simdgroup_float8x8 sgm1;"));
    check(has(metal, "simdgroup_load(sgm1, s0 + "));
    check(has(metal, "simdgroup_load(sgm2, buffer0 + "));
    check(has(metal, "simdgroup_multiply_accumulate(sgm0, sgm1, sgm2, sgm0);"));
    check(has(metal, "simdgroup_store(sgm0, s0 + "));
    check(has(metal, "simdgroup_store(sgm0, buffer1 + "));

    // Neither of the others has a wave matrix operation at the level eacp
    // targets, so a fragment is spread over the lanes the way Metal spreads
    // it: a pair of elements of each thread's own, at the row and the two
    // columns its lane within the SIMD group picks. The fill, the load and
    // the store move that pair, every lane its own, so nothing guards a
    // store; the product is the one operation that needs what other lanes
    // hold, and stages both operands through a scratch between two barriers.
    for (const auto& source: {hlsl, glsl})
    {
        check(has(source, "uint sgmRow = sgmLane / 4u;"));
        check(has(source, "uint sgmColumn = (sgmLane % 4u) * 2u;"));
        check(has(source, "sgm1 = "));
        check(has(source, "(s0["));
        check(has(source, "+ sgmRow * ("));
        check(has(source, "+ sgmColumn + 1u]);"));
        check(has(source, "(buffer0["));
        check(
            has(source, "sgmScratch[sgmBase + sgmRow * 8u + sgmColumn] = sgm1.x;"));
        check(has(source,
                  "sgmScratch[sgmBase + 64u + sgmRow * 8u + sgmColumn + 1u] = "
                  "sgm2.y;"));
        check(has(source, "for (uint sgm0k = 0u; sgm0k < 8u; ++sgm0k)"));
        check(
            has(source, "float sgm0l = sgmScratch[sgmBase + sgmRow * 8u + sgm0k];"));
        check(has(source,
                  "sgm0.x += sgm0l * sgmScratch[sgmBase + 64u + sgm0k * 8u + "
                  "sgmColumn];"));
        check(has(source,
                  "sgm0.y += sgm0l * sgmScratch[sgmBase + 64u + sgm0k * 8u + "
                  "sgmColumn + 1u];"));
        check(has(source, "+ sgmColumn] = sgm0.x;"));
        check(has(source, "+ sgmColumn + 1u] = sgm0.y;"));
        check(!has(source, "% 32u == 0u"));
        check(!has(source, "simdgroup_multiply_accumulate"));
    }

    // The two-vector each dialect spells a lane's pair as, and the scratch:
    // 128 threads are four SIMD groups, each with two fragments of its own.
    check(has(hlsl, "groupshared float sgmScratch[512];"));
    check(has(hlsl, "float2 sgm0 = float2(0.0, 0.0);"));
    check(has(hlsl, "float2 sgm1 = float2(s0["));
    check(has(glsl, "shared float sgmScratch[512];"));
    check(has(glsl, "vec2 sgm0 = vec2(0.0, 0.0);"));
    check(has(glsl, "vec2 sgm1 = vec2(s0["));

    // The kernel's own barrier and the two around the product's exchange.
    check(count(hlsl, "GroupMemoryBarrierWithGroupSync();") == 3);
    check(count(glsl, "barrier();") == 3);

    // The lane within the SIMD group, and the SIMD group's index, which Metal
    // has a builtin for and the other two divide the flat local index for.
    check(has(hlsl, "uint groupLane : SV_GroupIndex"));
    check(has(hlsl, "uint sgmLane = groupLane % 32u;"));
    check(has(hlsl, "uint sgmBase = (groupLane / 32u) * 128u;"));
    check(has(hlsl, "(groupLane / 32u)"));
    check(has(glsl, "uint sgmLane = gl_LocalInvocationIndex % 32u;"));
    check(has(glsl, "uint sgmBase = (gl_LocalInvocationIndex / 32u) * 128u;"));
    check(has(glsl, "(gl_LocalInvocationIndex / 32u)"));

    expectGlslCompiles(graph);
};

// A kernel that holds a fragment is a kernel that barriers, whatever else it
// does: an operation collective over a SIMD group cannot run where some of its
// lanes returned early, so the early-return bounds guard is not emitted and the
// kernel bounds its own stores.
auto tSimdMatrixHasNoGuard = test("SimdMatrix/aFragmentTakesTheBoundsGuardAway") = []
{
    auto builder = ShaderBuilder {};
    builder.setThreadGroupShape({64});

    auto a = builder.inputBuffer();
    auto output = builder.outputBuffer();
    auto id = builder.threadId();
    auto accumulator = builder.simdMatrix();

    builder.multiplyAccumulate(accumulator, accumulator, accumulator);
    builder.write(output, id, a[id]);

    const auto& graph = builder.graph();

    check(!has(emitMetal(graph), "if (gid >= uniforms.count)"));
    check(!has(emitHlsl(graph), "if (threadId.x >= uniforms.count)"));

    expectGlslCompiles(graph);
};

// The vocabulary belongs to the kernels that ask for it: one that never names a
// fragment declares none of the scaffolding either.
auto tNoFragmentNoScaffolding =
    test("SimdMatrix/aKernelWithoutOneDeclaresNothing") = []
{
    auto builder = ShaderBuilder {};

    auto a = builder.inputBuffer();
    auto output = builder.outputBuffer();
    auto id = builder.threadId();

    builder.write(output, id, a[id] * 2.f);

    const auto& graph = builder.graph();

    check(!has(emitMetal(graph), "simdgroup"));
    check(!has(emitMetal(graph), "simdIndex"));
    check(!has(emitHlsl(graph), "SV_GroupIndex"));
    check(!has(emitHlsl(graph), "sgm"));

    expectGlslCompiles(graph);
};

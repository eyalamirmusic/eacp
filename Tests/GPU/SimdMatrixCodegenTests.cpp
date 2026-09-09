#include "CodegenCommon.h"

// One 8x8 fragment, three dialects: MSL's own type and its three intrinsics on
// Metal, and a per-thread copy walked element by element on the two backends
// with no wave matrix operation to lower to.

using namespace nano;
using namespace eacp;
using namespace eacp::GPU;

namespace
{
auto has(const std::string& source, std::string_view text)
{
    return source.find(text) != std::string::npos;
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
    // targets, so a fragment is 64 floats of each thread's own and every
    // operation on one is a loop over them.
    for (const auto& source: {hlsl, glsl})
    {
        check(has(source, "float sgm0[64];"));
        check(has(source, "for (uint sgm0e = 0u; sgm0e < 64u; ++sgm0e)"));
        check(has(source, "sgm0[sgm0e] = 0.0;"));
        check(has(source, "float sgm1[64];"));
        check(has(source, "for (uint sgm1r = 0u; sgm1r < 8u; ++sgm1r)"));
        check(has(source, "for (uint sgm1c = 0u; sgm1c < 8u; ++sgm1c)"));
        check(has(source, "sgm1[sgm1r * 8u + sgm1c] = s0["));
        check(has(source, "sgm2[sgm2r * 8u + sgm2c] = buffer0["));
        check(has(source, "sgm0p[64];"));
        check(has(source,
                  "sgm0p[sgm0i * 8u + sgm0j] = sgm0[sgm0i * 8u + sgm0j] + "
                  "sgm0pe;"));
        check(!has(source, "simdgroup_multiply_accumulate"));
    }

    // The lane that makes a store: with every lane of what would have been a
    // SIMD group holding its own copy, letting all of them write would be the
    // same bytes 32 times over.
    check(has(hlsl, "uint groupLane : SV_GroupIndex"));
    check(has(hlsl, "if (groupLane % 32u == 0u)"));
    check(has(glsl, "if (gl_LocalInvocationIndex % 32u == 0u)"));

    // And the SIMD group's index, which Metal has a builtin for and the other
    // two divide the flat local index for.
    check(has(hlsl, "(groupLane / 32u)"));
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

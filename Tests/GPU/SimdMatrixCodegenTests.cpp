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

// A weight read where it lies: the activation staged as floats in a threadgroup
// tile, the weight loaded straight out of the buffer that holds it packed, and
// the two multiplied into a float accumulator with nothing widened in between.
ShaderBuilder packedKernel(SimdMatrixElement element)
{
    auto builder = ShaderBuilder {};
    builder.setThreadGroupShape({64});

    auto weights = builder.inputBuffer();
    auto output = builder.outputBuffer();
    auto tile = builder.shared<Float>(64);
    auto lane = builder.localId();
    auto simd = builder.simdGroupIndex();
    auto stride = builder.unsignedInteger(8u);

    builder.write(tile, lane, weights[lane]);
    builder.barrier();

    auto accumulator = builder.simdMatrix();
    auto left = builder.simdMatrix(tile, simd * 64u, stride);

    auto right = element == SimdMatrixElement::Half
                     ? builder.simdMatrixHalf(weights, simd * 64u, stride)
                     : builder.simdMatrixBFloat16(weights, simd * 64u, stride);

    builder.multiplyAccumulate(accumulator, left, right);
    builder.write(output, simd * 64u, stride, accumulator);

    return builder;
}
// The same product with a barrier between the loads and it. Another lane may
// have written that memory since, so the fragments are no longer what it
// holds and the product has to stage them through the scratch.
ShaderBuilder stagedProductKernel()
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

    builder.barrier();
    builder.multiplyAccumulate(accumulator, left, right);
    builder.write(output, simd * 64u, builder.unsignedInteger(8u), accumulator);

    return builder;
}
} // namespace

// The staged route, which is what a product falls back to when it cannot read
// its operands where they lie. Both barriers come back with it, and so does
// the scratch: a lane needs the row and columns other lanes hold, and the only
// place off Metal to put them is threadgroup memory.
auto tSimdMatrixStagedProduct = test("SimdMatrix/aProductStagesWhatItCannotRead") = []
{
    auto builder = stagedProductKernel();

    const auto& graph = builder.graph();
    auto hlsl = emitHlsl(graph);
    auto glsl = emitGlsl(graph);

    for (const auto& source: {hlsl, glsl})
    {
        check(
            has(source, "sgmScratch[sgmBase + sgmRow * 8u + sgmColumn] = sgm1.x;"));
        check(has(source,
                  "sgmScratch[sgmBase + 64u + sgmRow * 8u + sgmColumn + 1u] = "
                  "sgm2.y;"));
        check(
            has(source, "float sgm0l = sgmScratch[sgmBase + sgmRow * 8u + sgm0k];"));
        check(has(source,
                  "sgm0.x += sgm0l * sgmScratch[sgmBase + 64u + sgm0k * 8u + "
                  "sgmColumn];"));
    }

    // Staging is what the scratch is for, so here it is declared.
    check(has(hlsl, "groupshared float sgmScratch[512];"));
    check(has(glsl, "shared float sgmScratch[512];"));

    // The kernel's two, and the two the exchange puts around itself.
    check(count(hlsl, "GroupMemoryBarrierWithGroupSync();") == 4);
    check(count(glsl, "barrier();") == 4);

    expectGlslCompiles(graph);
};

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
    // store. The product is the one operation that needs what other lanes
    // hold - but both its operands are still standing where they were read,
    // so it takes their elements from there, k ascending, and needs neither
    // the scratch nor a barrier.
    for (const auto& source: {hlsl, glsl})
    {
        check(has(source, "uint sgmRow = sgmLane / 4u;"));
        check(has(source, "uint sgmColumn = (sgmLane % 4u) * 2u;"));
        check(has(source, "sgm1 = "));
        check(has(source, "(s0["));
        check(has(source, "+ sgmRow * ("));
        check(has(source, "+ sgmColumn + 1u]);"));
        check(has(source, "(buffer0["));
        check(has(source, "for (uint sgm0k = 0u; sgm0k < 8u; ++sgm0k)"));
        check(has(source, "float sgm0l = s0["));
        check(has(source, "+ sgmRow * (8u) + sgm0k];"));
        check(has(source, "sgm0.x += sgm0l * buffer0["));
        check(has(source, "+ sgm0k * (8u) + sgmColumn];"));
        check(has(source, "sgm0.y += sgm0l * buffer0["));
        check(has(source, "+ sgm0k * (8u) + sgmColumn + 1u];"));
        check(!has(source, "sgmScratch"));
        check(has(source, "+ sgmColumn] = sgm0.x;"));
        check(has(source, "+ sgmColumn + 1u] = sgm0.y;"));
        check(!has(source, "% 32u == 0u"));
        check(!has(source, "simdgroup_multiply_accumulate"));
    }

    // The two-vector each dialect spells a lane's pair as. No scratch is
    // declared at all: nothing staged anything into it, and threadgroup
    // memory a kernel does not use still costs it occupancy.
    check(!has(hlsl, "sgmScratch"));
    check(has(hlsl, "float2 sgm0 = float2(0.0, 0.0);"));
    check(has(hlsl, "float2 sgm1 = float2(s0["));
    check(!has(glsl, "sgmScratch"));
    check(has(glsl, "vec2 sgm0 = vec2(0.0, 0.0);"));
    check(has(glsl, "vec2 sgm1 = vec2(s0["));

    // The kernel's own barrier, and no others: the product added none.
    check(count(hlsl, "GroupMemoryBarrierWithGroupSync();") == 1);
    check(count(glsl, "barrier();") == 1);

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

// A fragment read out of a buffer of packed bf16. On Metal it is the type MSL
// has for one, loaded through the buffer's pointer reinterpreted as bfloat and
// multiplied into a float accumulator with nothing widened in between - which
// is what a packed load is for, since staging is the whole cost it removes.
auto tBFloat16Fragment = test("SimdMatrix/bfloat16LoadsWithoutStaging") = []
{
    auto builder = packedKernel(SimdMatrixElement::BFloat16);

    const auto& graph = builder.graph();
    auto metal = emitMetal(graph);
    auto hlsl = emitHlsl(graph);
    auto glsl = emitGlsl(graph);

    check(has(metal, "simdgroup_bfloat8x8 sgm2;"));
    check(has(metal,
              "simdgroup_load(sgm2, (device const bfloat*) (buffer0) + (t0), "
              "(8u));"));
    check(has(metal, "simdgroup_multiply_accumulate(sgm0, sgm1, sgm2, sgm0);"));

    // The float operand beside it is untouched, which is the mixed-precision
    // product the instruction takes: a staged activation against a packed
    // weight, accumulating in float.
    check(has(metal, "simdgroup_float8x8 sgm1;"));
    check(has(metal, "simdgroup_load(sgm1, s0 + (t0), (8u));"));
    check(!has(metal, "eacpReadBFloat16"));

    // The two fallback backends hold every fragment as a lane's pair of floats
    // whatever the memory it came from, so what the packed load changes there
    // is only the arithmetic that produces the pair: the word at half the
    // element's index, and which half of it the parity picks - the same helper
    // a scalar readBFloat16 goes through, and the definition carried with it.
    check(has(hlsl, "float eacpReadBFloat16(uint bits, uint parity)"));
    check(has(hlsl,
              "float2 sgm2 = float2(eacpReadBFloat16(asuint(buffer0[((t0) + "
              "sgmRow * (8u) + sgmColumn) / 2u]), ((t0) + sgmRow * (8u) + "
              "sgmColumn) % 2u), eacpReadBFloat16(asuint(buffer0[((t0) + sgmRow "
              "* (8u) + sgmColumn + 1u) / 2u]), ((t0) + sgmRow * (8u) + "
              "sgmColumn + 1u) % 2u));"));

    check(has(glsl, "float eacpReadBFloat16(uint bits, uint parity)"));
    check(has(glsl,
              "vec2 sgm2 = vec2(eacpReadBFloat16(floatBitsToUint(buffer0[((t0) "
              "+ sgmRow * (8u) + sgmColumn) / 2u])"));

    // And the product reads the weight where it lies, as a float operand
    // does: the widening helper again, now at the product's own index, so a
    // packed weight costs the scratch and its barriers no more than an
    // unpacked one does. The fp32 multiply below it is the one an fp32 patch
    // of the same values would have done.
    for (const auto& source: {hlsl, glsl})
    {
        check(has(source, "for (uint sgm0k = 0u; sgm0k < 8u; ++sgm0k)"));
        check(has(source, "sgm0.x += sgm0l * eacpReadBFloat16("));
        check(!has(source, "sgmScratch"));
        check(!has(source, "bfloat"));
    }

    expectGlslCompiles(graph);
};

// The fp16 sibling, which differs in the two names and nothing else - and is
// the one of the pair that is on eacp's macOS floor rather than above it.
auto tHalfFragment = test("SimdMatrix/halfLoadsWithoutStaging") = []
{
    auto builder = packedKernel(SimdMatrixElement::Half);

    const auto& graph = builder.graph();
    auto metal = emitMetal(graph);
    auto hlsl = emitHlsl(graph);

    check(has(metal, "simdgroup_half8x8 sgm2;"));
    check(has(metal,
              "simdgroup_load(sgm2, (device const half*) (buffer0) + (t0), (8u));"));
    check(has(metal, "simdgroup_multiply_accumulate(sgm0, sgm1, sgm2, sgm0);"));

    check(has(hlsl, "float eacpReadHalf(uint bits, uint parity)"));
    check(has(hlsl,
              "float2 sgm2 = float2(eacpReadHalf(asuint(buffer0[((t0) + sgmRow "
              "* (8u) + sgmColumn) / 2u])"));
    check(!has(hlsl, "eacpReadBFloat16"));

    expectGlslCompiles(graph);
};

// The packed element is the fragment's, not the kernel's: a kernel holding both
// kinds declares each as what it is, and carries only the widening its own
// fallback needs.
auto tPackedElementIsPerFragment =
    test("SimdMatrix/eachFragmentCarriesItsOwnElement") = []
{
    auto builder = ShaderBuilder {};
    builder.setThreadGroupShape({64});

    auto weights = builder.inputBuffer();
    auto output = builder.outputBuffer();
    auto simd = builder.simdGroupIndex();
    auto stride = builder.unsignedInteger(8u);

    auto accumulator = builder.simdMatrix();
    auto left = builder.simdMatrixHalf(weights, simd * 64u, stride);
    auto right = builder.simdMatrixBFloat16(weights, simd * 64u, stride);

    builder.multiplyAccumulate(accumulator, left, right);
    builder.write(output, simd * 64u, stride, accumulator);

    const auto& graph = builder.graph();

    check(graph.simdMatrixElement(0) == SimdMatrixElement::Float);
    check(graph.simdMatrixElement(1) == SimdMatrixElement::Half);
    check(graph.simdMatrixElement(2) == SimdMatrixElement::BFloat16);
    check(graph.usesPackedSimdMatrix(SimdMatrixElement::Half));
    check(graph.usesPackedSimdMatrix(SimdMatrixElement::BFloat16));

    auto metal = emitMetal(graph);

    check(has(metal, "simdgroup_half8x8 sgm1;"));
    check(has(metal, "simdgroup_bfloat8x8 sgm2;"));
    check(has(metal, "simdgroup_multiply_accumulate(sgm0, sgm1, sgm2, sgm0);"));

    auto hlsl = emitHlsl(graph);

    check(has(hlsl, "float eacpReadHalf(uint bits, uint parity)"));
    check(has(hlsl, "float eacpReadBFloat16(uint bits, uint parity)"));

    expectGlslCompiles(graph);
};

// A kernel that loads no packed fragment answers no to both, which is what
// makes ComputeProgram::fitsPackedSimdMatrix true everywhere for one.
auto tPlainFragmentNeedsNothing =
    test("SimdMatrix/aFloatFragmentAsksForNothing") = []
{
    auto builder = productKernel();

    const auto& graph = builder.graph();

    check(!graph.usesPackedSimdMatrix(SimdMatrixElement::Half));
    check(!graph.usesPackedSimdMatrix(SimdMatrixElement::BFloat16));
    check(!has(emitMetal(graph), "bfloat"));
    check(!has(emitHlsl(graph), "eacpReadBFloat16"));
};

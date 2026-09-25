#include "CodegenCommon.h"

#include <array>
#include <bit>
#include <cstdint>

// One 8x8 fragment, three dialects: MSL's own type and its three intrinsics on
// Metal, and a pair of elements per lane exchanged through threadgroup memory
// on the two backends with no wave matrix operation to lower to. The `…/runs`
// cases run the same graphs on the CPU executor, which holds a fragment whole
// per SIMD group and widens a packed one as it loads.

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
struct ProductKernel
{
    static constexpr auto threads = 128;

    ProductKernel()
    {
        builder.setThreadGroupShape({threads});

        a = builder.inputBuffer();
        output = builder.outputBuffer();
        auto tile = builder.shared<Float>(1024);
        auto lane = builder.localId();
        auto simd = builder.simdGroupIndex();

        builder.write(tile, lane, a[lane]);
        builder.barrier();

        auto accumulator = builder.simdMatrix();
        auto left =
            builder.simdMatrix(tile, simd * 64u, builder.unsignedInteger(8u));
        auto right = builder.simdMatrix(a, simd * 64u, builder.unsignedInteger(8u));

        builder.multiplyAccumulate(accumulator, left, right);

        builder.write(tile, simd * 64u, builder.unsignedInteger(8u), accumulator);
        builder.write(output, simd * 64u, builder.unsignedInteger(8u), accumulator);
    }

    ProductKernel(const ProductKernel&) = delete;
    ProductKernel& operator=(const ProductKernel&) = delete;

    const ShaderGraph& graph() const { return builder.graph(); }

    ShaderBuilder builder;
    InputBuffer a;
    OutputBuffer output;
};

// A kernel whose only fragment work is a zero squared into itself, beside an
// element copy: what it shows is the guard a fragment takes away.
struct UnguardedKernel
{
    UnguardedKernel()
    {
        builder.setThreadGroupShape({64});

        a = builder.inputBuffer();
        output = builder.outputBuffer();
        auto id = builder.threadId();
        auto accumulator = builder.simdMatrix();

        builder.multiplyAccumulate(accumulator, accumulator, accumulator);
        builder.write(output, id, a[id]);
    }

    UnguardedKernel(const UnguardedKernel&) = delete;
    UnguardedKernel& operator=(const UnguardedKernel&) = delete;

    const ShaderGraph& graph() const { return builder.graph(); }

    ShaderBuilder builder;
    InputBuffer a;
    OutputBuffer output;
};

using Fragment = std::array<float, 64>;

float scattered(int index)
{
    return (float) (((index * 37 + 5) % 23) - 11) * 0.1f;
}

// An 8x8 patch at `offset`, row stride 8, reading 0 past the end of `values`.
Fragment patchOf(const eacp::Vector<float>& values, int offset)
{
    auto patch = Fragment {};

    for (auto at = 0; at < 64; ++at)
        patch[at] = offset + at < values.size() ? values[offset + at] : 0.f;

    return patch;
}

// C + A * B, summed as the fallback sums it: the accumulator first, then k = 0
// to 7, one rounding per term.
Fragment multiplyAdd(const Fragment& accumulator,
                     const Fragment& left,
                     const Fragment& right)
{
    auto result = Fragment {};

    for (auto row = 0; row < 8; ++row)
        for (auto column = 0; column < 8; ++column)
        {
            auto sum = accumulator[row * 8 + column];

            for (auto k = 0; k < 8; ++k)
            {
                auto term = left[row * 8 + k] * right[k * 8 + column];
                sum += term;
            }

            result[row * 8 + column] = sum;
        }

    return result;
}

// A weight read where it lies: the activation staged as floats in a threadgroup
// tile, the weight loaded straight out of the buffer that holds it packed, and
// the two multiplied into a float accumulator with nothing widened in between.
struct PackedKernel
{
    static constexpr auto threads = 64;

    explicit PackedKernel(SimdMatrixElement element)
    {
        builder.setThreadGroupShape({threads});

        weights = builder.inputBuffer();
        output = builder.outputBuffer();
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
    }

    PackedKernel(const PackedKernel&) = delete;
    PackedKernel& operator=(const PackedKernel&) = delete;

    const ShaderGraph& graph() const { return builder.graph(); }

    ShaderBuilder builder;
    InputBuffer weights;
    OutputBuffer output;
};

// Packed element `index` of `words` as the fallback reads it: half `index % 2`
// of word `index / 2`, widened through the helper the shader's own
// eacpReadHalf or eacpReadBFloat16 is the twin of.
float widenedElement(SimdMatrixElement element,
                     const eacp::Vector<float>& words,
                     int index)
{
    auto bits = std::bit_cast<std::uint32_t>(words[index / 2]);
    auto parity = (std::uint32_t) (index % 2);

    return element == SimdMatrixElement::Half
               ? CpuCompute::readHalf(bits, parity)
               : CpuCompute::readBFloat16(bits, parity);
}
} // namespace

auto tSimdMatrixSource = test("SimdMatrix/eachBackendSpellsItsOwnWay") = []
{
    auto kernel = ProductKernel {};

    const auto& graph = kernel.graph();
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

// Four SIMD groups, each its own 8x8 product. The first two multiply the patch
// they staged by the same patch read from the buffer; the last two load their
// left operand from the part of the tile no lane wrote - undefined on a GPU,
// zeroed at group start on the CPU (D7) - so their product is 0. The case
// relies on that zero fill, which is why the graph runs only here. Nothing past
// the four patches is touched.
auto tSimdMatrixRuns = test("SimdMatrix/eachBackendSpellsItsOwnWay/runs") = []
{
    constexpr auto simdGroups = ProductKernel::threads / 32;
    constexpr auto written = simdGroups * 64;

    auto kernel = ProductKernel {};
    auto executor = CpuCompute::Executor {kernel.graph()};
    expectPlans(executor);

    auto a = eacp::Vector<float> {};

    for (auto i = 0; i < written; ++i)
        a.add(scattered(i));

    auto output = filledWith(written + 8, -1.f);

    auto bindings = CpuCompute::Bindings {};
    bindings.set(kernel.a, a);
    bindings.set(kernel.output, output);
    check(executor.dispatch(bindings, ProductKernel::threads));

    for (auto simd = 0; simd < simdGroups; ++simd)
    {
        auto staged = ProductKernel::threads;
        auto offset = simd * 64;
        auto left = offset < staged ? patchOf(a, offset) : Fragment {};
        auto expected = multiplyAdd(Fragment {}, left, patchOf(a, offset));
        auto matching = 0;

        for (auto at = 0; at < 64; ++at)
            matching += output[offset + at] == expected[at] ? 1 : 0;

        check(matching == 64, std::to_string(simd));
    }

    for (auto i = written; i < written + 8; ++i)
        check(output[i] == -1.f);
};

// A kernel that holds a fragment is a kernel that barriers, whatever else it
// does: an operation collective over a SIMD group cannot run where some of its
// lanes returned early, so the early-return bounds guard is not emitted and the
// kernel bounds its own stores.
auto tSimdMatrixHasNoGuard = test("SimdMatrix/aFragmentTakesTheBoundsGuardAway") = []
{
    auto kernel = UnguardedKernel {};

    const auto& graph = kernel.graph();

    check(!has(emitMetal(graph), "if (gid >= uniforms.count)"));
    check(!has(emitHlsl(graph), "if (threadId.x >= uniforms.count)"));

    expectGlslCompiles(graph);
};

// With no guard every lane of the last group runs: the ones past the extent
// read 0 and store it wherever the output has room, and nowhere else.
auto tSimdMatrixHasNoGuardRuns =
    test("SimdMatrix/aFragmentTakesTheBoundsGuardAway/runs") = []
{
    constexpr auto count = 100;
    constexpr auto room = 128;

    auto kernel = UnguardedKernel {};
    auto executor = CpuCompute::Executor {kernel.graph()};
    expectPlans(executor);

    auto a = eacp::Vector<float> {};

    for (auto i = 0; i < count; ++i)
        a.add(scattered(i));

    auto output = filledWith(room + 8, -1.f);

    auto bindings = CpuCompute::Bindings {};
    bindings.set(kernel.a, a);
    bindings.set(kernel.output, output);
    check(executor.dispatch(bindings, count));

    for (auto i = 0; i < room + 8; ++i)
    {
        auto expected = i < count ? a[i] : i < room ? 0.f : -1.f;
        check(output[i] == expected, std::to_string(i));
    }
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
    auto kernel = PackedKernel {SimdMatrixElement::BFloat16};

    const auto& graph = kernel.graph();
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

    // The product below it is the one the float pair always had: what a lane
    // holds is two floats either way.
    for (const auto& source: {hlsl, glsl})
    {
        check(has(source,
                  "sgmScratch[sgmBase + 64u + sgmRow * 8u + sgmColumn] = "
                  "sgm2.x;"));
        check(!has(source, "bfloat"));
    }

    expectGlslCompiles(graph);
};

// The fp16 sibling, which differs in the two names and nothing else - and is
// the one of the pair that is on eacp's macOS floor rather than above it.
auto tHalfFragment = test("SimdMatrix/halfLoadsWithoutStaging") = []
{
    auto kernel = PackedKernel {SimdMatrixElement::Half};

    const auto& graph = kernel.graph();
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

// The packed graphs on the CPU, which widens each element as it loads, through
// the twin of the helper the fallback calls. The weights are 128 packed values
// in 64 words; the tile stages those words as the floats their bits are, so the
// first SIMD group multiplies that float view by the packed patch. The second
// reads its left operand from past the end of the 64-element tile - undefined
// on a GPU, 0 on the CPU (D7) - so its product is 0; this relies on that.
namespace
{
void checkPackedRuns(SimdMatrixElement element)
{
    constexpr auto words = PackedKernel::threads;

    auto kernel = PackedKernel {element};
    auto executor = CpuCompute::Executor {kernel.graph()};
    expectPlans(executor);

    auto weights = eacp::Vector<float> {};

    for (auto word = 0; word < words; ++word)
    {
        auto low = scattered(2 * word);
        auto high = scattered(2 * word + 1);
        auto bits = element == SimdMatrixElement::Half
                        ? CpuCompute::packHalf2({low, high})
                        : CpuCompute::packBFloat16x2({low, high});
        weights.add(std::bit_cast<float>(bits));
    }

    auto output = filledWith(2 * 64 + 8, -1.f);

    auto bindings = CpuCompute::Bindings {};
    bindings.set(kernel.weights, weights);
    bindings.set(kernel.output, output);
    check(executor.dispatch(bindings, PackedKernel::threads));

    for (auto simd = 0; simd < 2; ++simd)
    {
        auto right = Fragment {};

        for (auto at = 0; at < 64; ++at)
            right[at] = widenedElement(element, weights, simd * 64 + at);

        auto left = simd == 0 ? patchOf(weights, 0) : Fragment {};
        auto expected = multiplyAdd(Fragment {}, left, right);
        auto matching = 0;

        for (auto at = 0; at < 64; ++at)
            matching += output[simd * 64 + at] == expected[at] ? 1 : 0;

        check(matching == 64, std::to_string(simd));
    }

    for (auto i = 2 * 64; i < 2 * 64 + 8; ++i)
        check(output[i] == -1.f);
}
} // namespace

auto tBFloat16FragmentRuns = test("SimdMatrix/bfloat16LoadsWithoutStaging/runs") = []
{ checkPackedRuns(SimdMatrixElement::BFloat16); };

auto tHalfFragmentRuns = test("SimdMatrix/halfLoadsWithoutStaging/runs") = []
{ checkPackedRuns(SimdMatrixElement::Half); };

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
    auto kernel = ProductKernel {};

    const auto& graph = kernel.graph();

    check(!graph.usesPackedSimdMatrix(SimdMatrixElement::Half));
    check(!graph.usesPackedSimdMatrix(SimdMatrixElement::BFloat16));
    check(!has(emitMetal(graph), "bfloat"));
    check(!has(emitHlsl(graph), "eacpReadBFloat16"));
};

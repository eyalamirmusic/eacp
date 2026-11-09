#include "CodegenCommon.h"

#include <initializer_list>
#include <string>

// The WGSL built-ins the EDSL gained beside its own set: the inverse
// hyperbolics, fma, degrees and radians, quantizeToF16, the bit functions and
// the normalized packs. Each is checked as text in all three dialects, and the
// GLSL is compiled wherever eacp-spirv is built.

using namespace nano;
using namespace eacp;
using namespace eacp::GPU;

namespace WgslBuiltinCodegenTests
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

// A kernel over one float and one uint buffer, so every function below has an
// argument of each family at hand.
struct FloatKernel
{
    FloatKernel()
    {
        input = builder.inputBuffer();
        output = builder.outputBuffer();
        id = builder.threadId();
    }

    ShaderBuilder builder;
    InputBuffer input;
    OutputBuffer output;
    UInt id;
};

struct IntegerKernel
{
    IntegerKernel()
    {
        input = builder.uintInputBuffer();
        output = builder.uintOutputBuffer();
        id = builder.threadId();
    }

    ShaderBuilder builder;
    UIntInputBuffer input;
    UIntOutputBuffer output;
    UInt id;
};
} // namespace

auto tInverseHyperbolics =
    test("WgslBuiltins/inverseHyperbolicsAreNativeOrAHelper") = []
{
    auto kernel = FloatKernel {};
    auto x = kernel.input[kernel.id];

    kernel.builder.write(kernel.output,
                         kernel.id,
                         asinh(x) + acosh(x) + atanh(x)
                             + asinh(kernel.input.read4(kernel.id)).w());

    auto sources = Sources {kernel.builder.graph()};

    for (const auto& native: {sources.metal, sources.glsl})
    {
        check(contains(native, "acosh("));
        check(contains(native, "atanh("));
        check(!contains(native, "eacpAcosh"));
        check(!contains(native, "eacpAtanh"));
    }

    // Metal's own asinh cancels for a negative argument under fast math, so
    // there it is taken of the magnitude and given the sign back.
    check(contains(sources.metal, "float4 eacpAsinh(float4 x)"));
    check(contains(sources.metal, "return copysign(asinh(abs(x)), x);"));
    check(contains(sources.glsl, "asinh("));
    check(!contains(sources.glsl, "eacpAsinh"));

    check(contains(sources.hlsl, "float eacpAsinh(float x)"));
    check(contains(sources.hlsl, "float4 eacpAsinh(float4 x)"));
    check(contains(sources.hlsl, "float eacpAcosh(float x)"));
    check(contains(sources.hlsl, "float eacpAtanh(float x)"));
    check(contains(sources.hlsl, "log(a + sqrt(a * a + 1.0))"));
    check(contains(sources.hlsl, "0.5 * log((1.0 + x) / (1.0 - x))"));
    check(!contains(sources.hlsl, " asinh("));

    expectGlslCompiles(kernel.builder.graph());
};

// HLSL's fma is double-only, so a float one is mad; MSL and GLSL want every
// argument the result's shape, and HLSL promotes a scalar itself.
auto tFma = test("WgslBuiltins/fmaIsMadOnHlslAndWidensItsScalars") = []
{
    auto kernel = FloatKernel {};
    auto v = kernel.input.read3(kernel.id);
    auto s = kernel.input[kernel.id];

    kernel.builder.write(
        kernel.output, kernel.id, fma(v, s, 2.0f).x() + fma(s, s, s));

    auto sources = Sources {kernel.builder.graph()};

    check(contains(sources.metal, "fma("));
    check(contains(sources.metal, "float3("));
    check(contains(sources.glsl, "fma("));
    check(contains(sources.glsl, "vec3("));
    check(contains(sources.hlsl, "mad("));
    check(!contains(sources.hlsl, "fma("));

    expectGlslCompiles(kernel.builder.graph());
};

// No dialect call at all: the multiplication WGSL defines them by.
auto tAngles = test("WgslBuiltins/degreesAndRadiansAreAMultiplication") = []
{
    auto kernel = FloatKernel {};
    auto x = kernel.input[kernel.id];

    kernel.builder.write(kernel.output, kernel.id, degrees(x) + radians(x));

    auto sources = Sources {kernel.builder.graph()};

    for (const auto* source: {&sources.metal, &sources.hlsl, &sources.glsl})
    {
        check(!contains(*source, "degrees("));
        check(!contains(*source, "radians("));
        check(contains(*source, "57.29578"));
        check(contains(*source, "0.017453292"));
    }

    check(degreesPerRadian == 57.29578f);
    check(radiansPerDegree == 0.017453292f);

    expectGlslCompiles(kernel.builder.graph());
};

auto tQuantize = test("WgslBuiltins/quantizeToF16RoundTripsThroughAHalf") = []
{
    auto kernel = FloatKernel {};

    kernel.builder.write(kernel.output,
                         kernel.id,
                         quantizeToF16(kernel.input[kernel.id])
                             + quantizeToF16(kernel.input.read3(kernel.id)).z());

    auto sources = Sources {kernel.builder.graph()};

    check(contains(sources.metal, "return float(half(x));"));
    check(contains(sources.metal, "return float3(half3(x));"));
    check(contains(sources.hlsl, "return f16tof32(f32tof16(x));"));
    check(contains(sources.glsl, "roundEven(uintBitsToFloat(magnitude)"));
    check(!contains(sources.glsl, "packHalf2x16"));
    check(contains(sources.glsl, "vec3 eacpQuantizeToF16(vec3 x)"));

    expectGlslCompiles(kernel.builder.graph());
};

// One helper per function, the uint and int overloads always both and the
// vector ones only for the widths called.
auto tBitHelpers = test("WgslBuiltins/bitFunctionsAreHelpersPerWidthUsed") = []
{
    auto kernel = IntegerKernel {};
    auto word = kernel.input[kernel.id];
    auto quad = kernel.input.read4(kernel.id);
    auto signedPair = toInt(kernel.input.read2(kernel.id));

    auto sum = countOneBits(word) + reverseBits(word) + countLeadingZeros(word)
               + countTrailingZeros(word) + firstLeadingBit(word)
               + firstTrailingBit(word) + extractBits(word, 3u, 7u)
               + insertBits(word, word >> 4u, kernel.id, 12u);

    auto vectorSum = countOneBits(quad) + firstLeadingBit(quad)
                     + extractBits(quad, kernel.id, 5u)
                     + insertBits(quad, quad, 1u, 30u);

    auto signedSum = firstLeadingBit(signedPair) + extractBits(signedPair, 4u, 8u)
                     + countTrailingZeros(toInt(word));

    kernel.builder.write(
        kernel.output, kernel.id, sum + vectorSum.x() + toUInt(signedSum).y());

    auto sources = Sources {kernel.builder.graph()};

    for (const auto& name: {"eacpCountOneBits",
                            "eacpReverseBits",
                            "eacpCountLeadingZeros",
                            "eacpCountTrailingZeros",
                            "eacpFirstLeadingBit",
                            "eacpFirstTrailingBit"})
    {
        for (const auto* source: {&sources.metal, &sources.hlsl, &sources.glsl})
        {
            check(contains(*source, std::string("uint ") + name + "(uint x)"));
            check(contains(*source, std::string("int ") + name + "(int x)"));
        }
    }

    check(contains(sources.metal, "uint4 eacpCountOneBits(uint4 x)"));
    check(contains(sources.hlsl, "uint4 eacpFirstLeadingBit(uint4 x)"));
    check(contains(sources.glsl, "uvec4 eacpFirstLeadingBit(uvec4 x)"));
    check(contains(sources.glsl, "ivec2 eacpFirstLeadingBit(ivec2 x)"));
    check(contains(sources.metal,
                   "int2 eacpExtractBits(int2 x, uint offset, uint count)"));
    check(
        contains(sources.hlsl,
                 "uint4 eacpInsertBits(uint4 x, uint4 y, uint offset, uint count)"));
    check(contains(sources.glsl,
                   "return uvec4(eacpInsertBits(x.x, y.x, offset, count), "
                   "eacpInsertBits(x.y, y.y, offset, count), "
                   "eacpInsertBits(x.z, y.z, offset, count), "
                   "eacpInsertBits(x.w, y.w, offset, count));"));

    // Only the widths called.
    check(!contains(sources.metal, "uint2 eacpCountOneBits"));
    check(!contains(sources.metal, "\nint4 eacpFirstLeadingBit"));
    check(!contains(sources.hlsl, "uint3 eacpExtractBits"));

    // The uint overload ahead of the int one that calls it.
    check(sources.hlsl.find("uint eacpInsertBits(uint x")
          < sources.hlsl.find("int eacpInsertBits(int x"));

    expectGlslCompiles(kernel.builder.graph());
};

// The spellings each dialect's corner cases come down to.
auto tBitSpellings = test("WgslBuiltins/bitFunctionsSettleTheirCornerCases") = []
{
    auto kernel = IntegerKernel {};
    auto word = kernel.input[kernel.id];
    auto signedWord = toInt(word);

    kernel.builder.write(kernel.output,
                         kernel.id,
                         countOneBits(word) + reverseBits(word)
                             + countLeadingZeros(word) + countTrailingZeros(word)
                             + firstLeadingBit(word) + firstTrailingBit(word)
                             + extractBits(word, 1u, 2u)
                             + insertBits(word, word, 1u, 2u)
                             + toUInt(extractBits(signedWord, kernel.id, word)));

    auto sources = Sources {kernel.builder.graph()};

    check(contains(sources.metal, "return popcount(x);"));
    check(contains(sources.metal, "return reverse_bits(x);"));
    check(contains(sources.metal, "return clz(x);"));
    check(contains(sources.metal, "return ctz(x);"));
    check(contains(sources.metal, "x == 0u ? 0xffffffffu : 31u - clz(x)"));

    check(contains(sources.hlsl, "return countbits(x);"));
    check(contains(sources.hlsl, "return reversebits(x);"));
    check(contains(sources.hlsl, "x == 0u ? 32u : 31u - firstbithigh(x)"));
    check(contains(sources.hlsl, "x == 0u ? 32u : firstbitlow(x)"));
    check(contains(sources.hlsl, "asint(asuint(x) << (32u - o - c)) >> (32u - c)"));

    check(contains(sources.glsl, "return uint(bitCount(x));"));
    check(contains(sources.glsl, "return bitfieldReverse(x);"));
    check(contains(sources.glsl, "x == 0u ? 32u : uint(31 - findMSB(x))"));
    check(contains(sources.glsl, "int(uint(x) << (32u - o - c)) >> (32u - c)"));

    for (const auto* source: {&sources.metal, &sources.hlsl, &sources.glsl})
    {
        check(contains(*source, "uint o = min(offset, 32u);"));
        check(contains(*source, "uint c = min(count, 32u - o);"));
        check(contains(*source, "(x >> o) & (0xffffffffu >> (32u - c))"));
        check(contains(*source, "uint mask = (0xffffffffu >> (32u - c)) << o;"));
        check(contains(*source, "x < 0 ? ~x : x"));
    }

    expectGlslCompiles(kernel.builder.graph());
};

// WGSL's floor(0.5 + scaled) in every dialect, never the dialect's own pack.
auto tNormalizedPacks = test("WgslBuiltins/normalizedPacksAreTheWgslFormula") = []
{
    auto kernel = FloatKernel {};
    auto quad = kernel.input.read4(kernel.id);
    auto pair = kernel.input.read2(kernel.id);

    auto packed = packUnorm8x4(quad) + packSnorm8x4(quad) + packUnorm16x2(pair)
                  + packSnorm16x2(pair);

    auto unpacked = unpackUnorm8x4(packed).x() + unpackSnorm8x4(packed).y()
                    + unpackUnorm16x2(packed).x() + unpackSnorm16x2(packed).y();

    kernel.builder.write(kernel.output, kernel.id, unpacked);

    auto sources = Sources {kernel.builder.graph()};

    for (const auto* source: {&sources.metal, &sources.hlsl, &sources.glsl})
    {
        for (const auto& name: {"eacpPackUnorm8x4",
                                "eacpPackSnorm8x4",
                                "eacpPackUnorm16x2",
                                "eacpPackSnorm16x2",
                                "eacpUnpackUnorm8x4",
                                "eacpUnpackSnorm8x4",
                                "eacpUnpackUnorm16x2",
                                "eacpUnpackSnorm16x2"})
            check(contains(*source, std::string(name) + "("));

        check(contains(*source, "floor(0.5 + 255.0 * "));
        check(contains(*source, "floor(0.5 + 127.0 * "));
        check(contains(*source, "floor(0.5 + 65535.0 * "));
        check(contains(*source, "floor(0.5 + 32767.0 * "));
        check(contains(*source, "/ 127.0, "));
        check(contains(*source, "/ 32767.0, "));
        check(!contains(*source, "pack_float_to"));
        check(!contains(*source, "packUnorm4x8"));
        check(!contains(*source, "packSnorm4x8"));
    }

    check(contains(sources.hlsl, "saturate(values)"));
    check(contains(sources.metal, "clamp(values, float4(-1.0), float4(1.0))"));
    check(contains(sources.glsl, "clamp(values, -1.0, 1.0)"));

    expectGlslCompiles(kernel.builder.graph());
};

// Nothing new is emitted into a kernel that calls none of it.
auto tNothingUnused = test("WgslBuiltins/helpersAreEmittedOnlyWhenCalled") = []
{
    auto kernel = FloatKernel {};
    kernel.builder.write(kernel.output, kernel.id, kernel.input[kernel.id] * 2.0f);

    auto sources = Sources {kernel.builder.graph()};

    for (const auto* source: {&sources.metal, &sources.hlsl, &sources.glsl})
    {
        check(!contains(*source, "eacpAsinh"));
        check(!contains(*source, "eacpQuantizeToF16"));
        check(!contains(*source, "eacpExtractBits"));
        check(!contains(*source, "eacpPackUnorm8x4"));
    }
};

// The CPU executor plans every one of them.
auto tPlans = test("WgslBuiltins/everyFunctionPlansOnTheCpu") = []
{
    auto floats = FloatKernel {};
    auto x = floats.input[floats.id];
    auto quad = floats.input.read4(floats.id);
    auto packed = packUnorm8x4(quad) + packSnorm8x4(quad) + packUnorm16x2(quad.xy())
                  + packSnorm16x2(quad.zw());

    floats.builder.write(
        floats.output,
        floats.id,
        asinh(x) + acosh(x) + atanh(x) + fma(x, x, 1.0f) + degrees(x) + radians(x)
            + quantizeToF16(x) + unpackUnorm8x4(packed).x()
            + unpackSnorm8x4(packed).y() + unpackUnorm16x2(packed).x()
            + unpackSnorm16x2(packed).y());

    expectPlans(CpuCompute::Executor {floats.builder.graph()});

    auto integers = IntegerKernel {};
    auto word = integers.input[integers.id];
    auto signedQuad = toInt(integers.input.read4(integers.id));

    integers.builder.write(
        integers.output,
        integers.id,
        countOneBits(word) + reverseBits(word) + countLeadingZeros(word)
            + countTrailingZeros(word) + firstLeadingBit(word)
            + firstTrailingBit(word) + extractBits(word, integers.id, 4u)
            + insertBits(word, word, 2u, integers.id)
            + toUInt(firstLeadingBit(signedQuad) + extractBits(signedQuad, 1u, 9u)
                     + insertBits(signedQuad, signedQuad, 3u, 3u))
                  .w());

    expectPlans(CpuCompute::Executor {integers.builder.graph()});
};
} // namespace WgslBuiltinCodegenTests

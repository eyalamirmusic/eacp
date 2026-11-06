#include "ShaderHelpers.h"

namespace eacp::GPU::ShaderEmission
{
namespace
{

// The builtins whose two spellings are not one name apart, emitted as a
// function definition ahead of the shader body and called like any other.
//
// callName above handles the ordinary case - one name, one argument list, a
// different word. This is for the rest: unpacking two halves out of a word is
// a bitcast and a vector conversion on MSL, and two f16tof32 calls against a
// shift on HLSL, and no renaming reconciles those. A helper does, and it keeps
// the graph backend-agnostic - one call node with one argument, both sides.
//
// Each definition stands alone and calls no other helper, because what is
// emitted is decided per helper by whether the graph names it: one that leaned
// on another would compile only when the graph happened to call both.
struct ShaderHelper
{
    const char* name;

    // Null where the dialect has the function natively and emits nothing for it.
    const char* metal;
    const char* directX;
    const char* glsl;
};

// The error function, as Abramowitz & Stegun 7.1.26.
//
// Neither language has one. HLSL under FXC never did, and MSL - despite being
// the side that usually has the richer math library - rejects a call to erf as
// an undeclared identifier, so this is the definition on both backends rather
// than the Windows half of a pair.
//
// That is why one string serves both: what the approximation is written out of
// - abs, exp, a divide, a Horner chain and the scalar conditionals - is spelled
// identically in MSL and HLSL, so there is nothing here for a per-backend form
// to differ about. The vector widths are overloads rather than a genType
// because HLSL resolves a user function by overload and has no template before
// shader model 6; MSL is C++ and accepts the overloads unchanged.
//
// Measured against std::erf over the whole real line: worst absolute error
// under 6e-7 for both, and 1.7e-7 for what Metal's own arithmetic makes of it.
// Tests/GPU/IntrinsicTests pins both. The approximation itself is good to
// 1.5e-7 and the rest is what evaluating it in float32 costs - which lands
// under the resolution a float has near one either way, so the shader is a
// float32 error function and not a rounded copy of the CPU's.
//
// The polynomial is not odd, and both signs of zero take its positive branch,
// so the origin returns the argument: erf(0) is exactly zero with the sign it
// was handed, and erf(-x) is bitwise the negation of erf(x).
constexpr auto erfHelper =
    "float eacpErf(float x)\n"
    "{\n"
    "    float a = abs(x);\n"
    "    float t = 1.0 / (1.0 + 0.3275911 * a);\n"
    "    float e = 1.0 - t * (0.254829592 + t * (-0.284496736 + t * (1.421413741\n"
    "              + t * (-1.453152027 + t * 1.061405429)))) * exp(-a * a);\n"
    "    return a == 0.0 ? x : (x < 0.0 ? -e : e);\n"
    "}\n\n"
    "float2 eacpErf(float2 x)\n"
    "{\n"
    "    return float2(eacpErf(x.x), eacpErf(x.y));\n"
    "}\n\n"
    "float3 eacpErf(float3 x)\n"
    "{\n"
    "    return float3(eacpErf(x.x), eacpErf(x.y), eacpErf(x.z));\n"
    "}\n\n"
    "float4 eacpErf(float4 x)\n"
    "{\n"
    "    return float4(eacpErf(x.x), eacpErf(x.y), eacpErf(x.z), eacpErf(x.w));\n"
    "}\n\n";

// Its complement, as poly(t) * exp(-x*x) rather than as 1 - eacpErf(x): erf has
// saturated at 1.0f by x = 4 while erfc there is still 1.5e-8, so the
// subtraction would return zero for the whole tail - which is the half of erfc
// that anything asks for it by name. What the direct form cannot fix is the
// approximation's own relative error out there, around 1% by x = 3, so this
// answers "how much probability is left" and not "to how many digits".
//
// The origin is answered outright for the same reason: erfc(0) is exactly 1,
// whichever sign of zero it was handed, so erf(x) + erfc(x) is exactly 1 there.
constexpr auto erfcHelper =
    "float eacpErfc(float x)\n"
    "{\n"
    "    float a = abs(x);\n"
    "    float t = 1.0 / (1.0 + 0.3275911 * a);\n"
    "    float e = t * (0.254829592 + t * (-0.284496736 + t * (1.421413741\n"
    "              + t * (-1.453152027 + t * 1.061405429)))) * exp(-a * a);\n"
    "    return a == 0.0 ? 1.0 : (x < 0.0 ? 2.0 - e : e);\n"
    "}\n\n"
    "float2 eacpErfc(float2 x)\n"
    "{\n"
    "    return float2(eacpErfc(x.x), eacpErfc(x.y));\n"
    "}\n\n"
    "float3 eacpErfc(float3 x)\n"
    "{\n"
    "    return float3(eacpErfc(x.x), eacpErfc(x.y), eacpErfc(x.z));\n"
    "}\n\n"
    "float4 eacpErfc(float4 x)\n"
    "{\n"
    "    return float4(eacpErfc(x.x), eacpErfc(x.y), eacpErfc(x.z), "
    "eacpErfc(x.w));\n"
    "}\n\n";

constexpr auto erfHelperGlsl =
    "float eacpErf(float x)\n"
    "{\n"
    "    float a = abs(x);\n"
    "    float t = 1.0 / (1.0 + 0.3275911 * a);\n"
    "    float e = 1.0 - t * (0.254829592 + t * (-0.284496736 + t * (1.421413741\n"
    "              + t * (-1.453152027 + t * 1.061405429)))) * exp(-a * a);\n"
    "    return a == 0.0 ? x : (x < 0.0 ? -e : e);\n"
    "}\n\n"
    "vec2 eacpErf(vec2 x)\n"
    "{\n"
    "    return vec2(eacpErf(x.x), eacpErf(x.y));\n"
    "}\n\n"
    "vec3 eacpErf(vec3 x)\n"
    "{\n"
    "    return vec3(eacpErf(x.x), eacpErf(x.y), eacpErf(x.z));\n"
    "}\n\n"
    "vec4 eacpErf(vec4 x)\n"
    "{\n"
    "    return vec4(eacpErf(x.x), eacpErf(x.y), eacpErf(x.z), eacpErf(x.w));\n"
    "}\n\n";

constexpr auto erfcHelperGlsl =
    "float eacpErfc(float x)\n"
    "{\n"
    "    float a = abs(x);\n"
    "    float t = 1.0 / (1.0 + 0.3275911 * a);\n"
    "    float e = t * (0.254829592 + t * (-0.284496736 + t * (1.421413741\n"
    "              + t * (-1.453152027 + t * 1.061405429)))) * exp(-a * a);\n"
    "    return a == 0.0 ? 1.0 : (x < 0.0 ? 2.0 - e : e);\n"
    "}\n\n"
    "vec2 eacpErfc(vec2 x)\n"
    "{\n"
    "    return vec2(eacpErfc(x.x), eacpErfc(x.y));\n"
    "}\n\n"
    "vec3 eacpErfc(vec3 x)\n"
    "{\n"
    "    return vec3(eacpErfc(x.x), eacpErfc(x.y), eacpErfc(x.z));\n"
    "}\n\n"
    "vec4 eacpErfc(vec4 x)\n"
    "{\n"
    "    return vec4(eacpErfc(x.x), eacpErfc(x.y), eacpErfc(x.z), "
    "eacpErfc(x.w));\n"
    "}\n\n";

// tanh with its two tails answered outright.
//
// The native one is there in all three languages, and in none of them is what
// it does to a large argument something a caller can rely on. Metal is the case
// that forced this: eacp compiles its library with no MTLCompileOptions, so
// fast math is on, and under it tanh is evaluated through exp - which overflows
// somewhere past an argument of 44 and hands back inf/inf, a NaN, where the
// function saturated at one long before. HLSL and GLSL leave the same freedom
// to the driver. A tanh GELU's argument is cubic in its input, so an activation
// of thirty is an argument of nine hundred, and one NaN in a residual stream is
// the rest of the sequence.
//
// Ten is the threshold because float32 resolves nothing between tanh(9.011) and
// one: 1 - tanh(x) falls below half an ulp of one there, so every argument this
// answers with a constant is an argument whose correctly rounded tanh is that
// same constant. The branch therefore changes no representable value of the
// function - it only keeps the intrinsic inside the range where a driver's
// version of it is worth calling.
constexpr auto saturatingTanhHelper =
    "float eacpSaturatingTanh(float x)\n"
    "{\n"
    "    return x >= 10.0 ? 1.0 : (x <= -10.0 ? -1.0 : tanh(x));\n"
    "}\n\n"
    "float2 eacpSaturatingTanh(float2 x)\n"
    "{\n"
    "    return float2(eacpSaturatingTanh(x.x), eacpSaturatingTanh(x.y));\n"
    "}\n\n"
    "float3 eacpSaturatingTanh(float3 x)\n"
    "{\n"
    "    return float3(eacpSaturatingTanh(x.x), eacpSaturatingTanh(x.y),\n"
    "                  eacpSaturatingTanh(x.z));\n"
    "}\n\n"
    "float4 eacpSaturatingTanh(float4 x)\n"
    "{\n"
    "    return float4(eacpSaturatingTanh(x.x), eacpSaturatingTanh(x.y),\n"
    "                  eacpSaturatingTanh(x.z), eacpSaturatingTanh(x.w));\n"
    "}\n\n";

constexpr auto saturatingTanhHelperGlsl =
    "float eacpSaturatingTanh(float x)\n"
    "{\n"
    "    return x >= 10.0 ? 1.0 : (x <= -10.0 ? -1.0 : tanh(x));\n"
    "}\n\n"
    "vec2 eacpSaturatingTanh(vec2 x)\n"
    "{\n"
    "    return vec2(eacpSaturatingTanh(x.x), eacpSaturatingTanh(x.y));\n"
    "}\n\n"
    "vec3 eacpSaturatingTanh(vec3 x)\n"
    "{\n"
    "    return vec3(eacpSaturatingTanh(x.x), eacpSaturatingTanh(x.y),\n"
    "                eacpSaturatingTanh(x.z));\n"
    "}\n\n"
    "vec4 eacpSaturatingTanh(vec4 x)\n"
    "{\n"
    "    return vec4(eacpSaturatingTanh(x.x), eacpSaturatingTanh(x.y),\n"
    "                eacpSaturatingTanh(x.z), eacpSaturatingTanh(x.w));\n"
    "}\n\n";

// log2 scaled by log10(2), which is what a driver's own log10 lowers to.
constexpr auto log10HelperGlsl = "float eacpLog10(float x)\n"
                                 "{\n"
                                 "    return log2(x) * 0.30102999566;\n"
                                 "}\n\n"
                                 "vec2 eacpLog10(vec2 x)\n"
                                 "{\n"
                                 "    return log2(x) * 0.30102999566;\n"
                                 "}\n\n"
                                 "vec3 eacpLog10(vec3 x)\n"
                                 "{\n"
                                 "    return log2(x) * 0.30102999566;\n"
                                 "}\n\n"
                                 "vec4 eacpLog10(vec4 x)\n"
                                 "{\n"
                                 "    return log2(x) * 0.30102999566;\n"
                                 "}\n\n";

// The bf16 narrowing, written in integer arithmetic on purpose.
//
// packHalf2 hands the rounding to the dialect's own narrowing instruction and
// so cannot be bit-identical across the three. Here there is no instruction to
// hand it to - no language has bf16 - and doing it by hand costs nothing and
// buys an answer that is the same on every backend: add half an ulp plus the
// low bit of what survives, which is round-to-nearest-even, then keep the top
// sixteen bits.
//
// A NaN is quieted rather than rounded. Adding to one whose mantissa is all
// ones carries into the exponent and lands on an infinity, which is a different
// value rather than a coarser one, so the two are told apart first: a magnitude
// above the infinity pattern is exactly a NaN.
constexpr auto packBFloat16HelperMetal =
    "inline uint eacpPackBFloat16x2(float2 values)\n"
    "{\n"
    "    uint low = as_type<uint>(values.x);\n"
    "    uint high = as_type<uint>(values.y);\n"
    "    bool lowIsNaN = (low & 0x7fffffffu) > 0x7f800000u;\n"
    "    bool highIsNaN = (high & 0x7fffffffu) > 0x7f800000u;\n"
    "    low = lowIsNaN ? (low | 0x400000u)\n"
    "                   : (low + 0x7fffu + ((low >> 16u) & 1u));\n"
    "    high = highIsNaN ? (high | 0x400000u)\n"
    "                     : (high + 0x7fffu + ((high >> 16u) & 1u));\n"
    "    return (low >> 16u) | (high & 0xffff0000u);\n"
    "}\n\n";

constexpr auto packBFloat16HelperHlsl =
    "uint eacpPackBFloat16x2(float2 values)\n"
    "{\n"
    "    uint low = asuint(values.x);\n"
    "    uint high = asuint(values.y);\n"
    "    bool lowIsNaN = (low & 0x7fffffffu) > 0x7f800000u;\n"
    "    bool highIsNaN = (high & 0x7fffffffu) > 0x7f800000u;\n"
    "    low = lowIsNaN ? (low | 0x400000u)\n"
    "                   : (low + 0x7fffu + ((low >> 16u) & 1u));\n"
    "    high = highIsNaN ? (high | 0x400000u)\n"
    "                     : (high + 0x7fffu + ((high >> 16u) & 1u));\n"
    "    return (low >> 16u) | (high & 0xffff0000u);\n"
    "}\n\n";

constexpr auto packBFloat16HelperGlsl =
    "uint eacpPackBFloat16x2(vec2 values)\n"
    "{\n"
    "    uint low = floatBitsToUint(values.x);\n"
    "    uint high = floatBitsToUint(values.y);\n"
    "    bool lowIsNaN = (low & 0x7fffffffu) > 0x7f800000u;\n"
    "    bool highIsNaN = (high & 0x7fffffffu) > 0x7f800000u;\n"
    "    low = lowIsNaN ? (low | 0x400000u)\n"
    "                   : (low + 0x7fffu + ((low >> 16u) & 1u));\n"
    "    high = highIsNaN ? (high | 0x400000u)\n"
    "                     : (high + 0x7fffu + ((high >> 16u) & 1u));\n"
    "    return (low >> 16u) | (high & 0xffff0000u);\n"
    "}\n\n";

// The quantized widenings, and the sign extension inside them, which is why
// these are helpers rather than arithmetic at the call site.
//
// A byte lifted out of a word is an unsigned 0..255, and the signed value it
// stands for is (b ^ 0x80) - 128; a nibble's is (n ^ 0x8) - 8. That spelling is
// arithmetic on values no wider than the word, moving no bit into or out of a
// sign position, so MSL, HLSL and GLSL define it identically - which is why one
// string serves Metal and DirectX here, as it does for erf. The alternatives do
// not: as_type<char4> is Metal's answer alone, and shifting a byte up into the
// sign bit and arithmetically back asks each language what its own sign bit
// does under a shift, which is three rules spelled three ways for a widening
// that has to agree to the bit.
constexpr auto unpackInt8x4Helper =
    "float4 eacpUnpackInt8x4(uint bits)\n"
    "{\n"
    "    return float4(float((bits & 0xffu) ^ 0x80u) - 128.0,\n"
    "                  float(((bits >> 8u) & 0xffu) ^ 0x80u) - 128.0,\n"
    "                  float(((bits >> 16u) & 0xffu) ^ 0x80u) - 128.0,\n"
    "                  float(((bits >> 24u) & 0xffu) ^ 0x80u) - 128.0);\n"
    "}\n\n";

constexpr auto unpackInt8x4HelperGlsl =
    "vec4 eacpUnpackInt8x4(uint bits)\n"
    "{\n"
    "    return vec4(float((bits & 0xffu) ^ 0x80u) - 128.0,\n"
    "                float(((bits >> 8u) & 0xffu) ^ 0x80u) - 128.0,\n"
    "                float(((bits >> 16u) & 0xffu) ^ 0x80u) - 128.0,\n"
    "                float(((bits >> 24u) & 0xffu) ^ 0x80u) - 128.0);\n"
    "}\n\n";

constexpr auto unpackUInt8x4Helper =
    "float4 eacpUnpackUInt8x4(uint bits)\n"
    "{\n"
    "    return float4(float(bits & 0xffu),\n"
    "                  float((bits >> 8u) & 0xffu),\n"
    "                  float((bits >> 16u) & 0xffu),\n"
    "                  float((bits >> 24u) & 0xffu));\n"
    "}\n\n";

constexpr auto unpackUInt8x4HelperGlsl =
    "vec4 eacpUnpackUInt8x4(uint bits)\n"
    "{\n"
    "    return vec4(float(bits & 0xffu),\n"
    "                float((bits >> 8u) & 0xffu),\n"
    "                float((bits >> 16u) & 0xffu),\n"
    "                float((bits >> 24u) & 0xffu));\n"
    "}\n\n";

// Four nibbles out of the low sixteen bits of a word. The high four are the
// same function of the word shifted down sixteen, which unpackInt4x8 records as
// a shift node rather than a second helper - so one definition covers both
// halves and the shift is visible in the emitted source.
constexpr auto unpackInt4x4Helper =
    "float4 eacpUnpackInt4x4(uint bits)\n"
    "{\n"
    "    return float4(float((bits & 0xfu) ^ 0x8u) - 8.0,\n"
    "                  float(((bits >> 4u) & 0xfu) ^ 0x8u) - 8.0,\n"
    "                  float(((bits >> 8u) & 0xfu) ^ 0x8u) - 8.0,\n"
    "                  float(((bits >> 12u) & 0xfu) ^ 0x8u) - 8.0);\n"
    "}\n\n";

constexpr auto unpackInt4x4HelperGlsl =
    "vec4 eacpUnpackInt4x4(uint bits)\n"
    "{\n"
    "    return vec4(float((bits & 0xfu) ^ 0x8u) - 8.0,\n"
    "                float(((bits >> 4u) & 0xfu) ^ 0x8u) - 8.0,\n"
    "                float(((bits >> 8u) & 0xfu) ^ 0x8u) - 8.0,\n"
    "                float(((bits >> 12u) & 0xfu) ^ 0x8u) - 8.0);\n"
    "}\n\n";

constexpr auto unpackUInt4x4Helper =
    "float4 eacpUnpackUInt4x4(uint bits)\n"
    "{\n"
    "    return float4(float(bits & 0xfu),\n"
    "                  float((bits >> 4u) & 0xfu),\n"
    "                  float((bits >> 8u) & 0xfu),\n"
    "                  float((bits >> 12u) & 0xfu));\n"
    "}\n\n";

constexpr auto unpackUInt4x4HelperGlsl =
    "vec4 eacpUnpackUInt4x4(uint bits)\n"
    "{\n"
    "    return vec4(float(bits & 0xfu),\n"
    "                float((bits >> 4u) & 0xfu),\n"
    "                float((bits >> 8u) & 0xfu),\n"
    "                float((bits >> 12u) & 0xfu));\n"
    "}\n\n";

// The inverse. Each component is masked to its low eight bits in the *signed*
// domain first, which every one of the three defines as the two's-complement
// pattern and which leaves a value in 0..255 - so the conversion to uint that
// follows is of a non-negative number and has nothing left to disagree about.
// Casting a negative int straight to uint would be the shorter spelling and the
// one whose result each language words differently.
constexpr auto packInt8x4Helper =
    "uint eacpPackInt8x4(int4 values)\n"
    "{\n"
    "    return uint(values.x & 0xff) | (uint(values.y & 0xff) << 8u)\n"
    "           | (uint(values.z & 0xff) << 16u)\n"
    "           | (uint(values.w & 0xff) << 24u);\n"
    "}\n\n";

constexpr auto packInt8x4HelperGlsl =
    "uint eacpPackInt8x4(ivec4 values)\n"
    "{\n"
    "    return uint(values.x & 0xff) | (uint(values.y & 0xff) << 8u)\n"
    "           | (uint(values.z & 0xff) << 16u)\n"
    "           | (uint(values.w & 0xff) << 24u);\n"
    "}\n\n";

constexpr auto packUInt8x4Helper =
    "uint eacpPackUInt8x4(uint4 values)\n"
    "{\n"
    "    return (values.x & 0xffu) | ((values.y & 0xffu) << 8u)\n"
    "           | ((values.z & 0xffu) << 16u)\n"
    "           | ((values.w & 0xffu) << 24u);\n"
    "}\n\n";

constexpr auto packUInt8x4HelperGlsl =
    "uint eacpPackUInt8x4(uvec4 values)\n"
    "{\n"
    "    return (values.x & 0xffu) | ((values.y & 0xffu) << 8u)\n"
    "           | ((values.z & 0xffu) << 16u)\n"
    "           | ((values.w & 0xffu) << 24u);\n"
    "}\n\n";

const auto shaderHelpers = Array<ShaderHelper, 18> {
    ShaderHelper {"eacpUnpackHalf2",
                  "inline float2 eacpUnpackHalf2(uint bits)\n"
                  "{\n"
                  "    return float2(as_type<half2>(bits));\n"
                  "}\n\n",
                  "float2 eacpUnpackHalf2(uint bits)\n"
                  "{\n"
                  "    return float2(f16tof32(bits), f16tof32(bits >> 16));\n"
                  "}\n\n",
                  "vec2 eacpUnpackHalf2(uint bits)\n"
                  "{\n"
                  "    return unpackHalf2x16(bits);\n"
                  "}\n\n"},
    ShaderHelper {"eacpErf", erfHelper, erfHelper, erfHelperGlsl},
    ShaderHelper {"eacpErfc", erfcHelper, erfcHelper, erfcHelperGlsl},
    ShaderHelper {"eacpSaturatingTanh",
                  saturatingTanhHelper,
                  saturatingTanhHelper,
                  saturatingTanhHelperGlsl},
    // One half chosen by a parity rather than both unpacked and one dropped:
    // shifting the wanted half down is a single instruction in every language.
    // as_type<half2>, f16tof32 and unpackHalf2x16 all read the low sixteen bits.
    ShaderHelper {"eacpReadHalf",
                  "inline float eacpReadHalf(uint bits, uint parity)\n"
                  "{\n"
                  "    return float(as_type<half2>(bits >> (16u * parity)).x);\n"
                  "}\n\n",
                  "float eacpReadHalf(uint bits, uint parity)\n"
                  "{\n"
                  "    return f16tof32(bits >> (16u * parity));\n"
                  "}\n\n",
                  "float eacpReadHalf(uint bits, uint parity)\n"
                  "{\n"
                  "    return unpackHalf2x16(bits >> (16u * parity)).x;\n"
                  "}\n\n"},

    // The narrowing. No mask on the low half: f32tof16 is specified to set the
    // upper sixteen bits of its result to zero, and the D3D11.1 spec says so
    // again as a clarification that it holds on all hardware supporting the
    // instruction.
    //
    // Not bit-identical across the three for a value fp16 cannot hold.
    ShaderHelper {"eacpPackHalf2",
                  "inline uint eacpPackHalf2(float2 values)\n"
                  "{\n"
                  "    return as_type<uint>(half2(values));\n"
                  "}\n\n",
                  "uint eacpPackHalf2(float2 values)\n"
                  "{\n"
                  "    return f32tof16(values.x) | (f32tof16(values.y) << 16u);\n"
                  "}\n\n",
                  "uint eacpPackHalf2(vec2 values)\n"
                  "{\n"
                  "    return packHalf2x16(values);\n"
                  "}\n\n"},

    // bf16 is fp32 with the low sixteen mantissa bits dropped, so widening one
    // is those bits put back at the top of a word - a shift and a bitcast, with
    // nothing to rebias and no subnormal case. The three dialects differ only
    // in how they spell the bitcast, and the result is bit-identical on all of
    // them. Deliberately not routed through fp16: five exponent bits against
    // bf16's eight would flush a small weight to zero and take a large one to
    // infinity.
    ShaderHelper {"eacpUnpackBFloat16x2",
                  "inline float2 eacpUnpackBFloat16x2(uint bits)\n"
                  "{\n"
                  "    return float2(as_type<float>(bits << 16u),\n"
                  "                  as_type<float>(bits & 0xffff0000u));\n"
                  "}\n\n",
                  "float2 eacpUnpackBFloat16x2(uint bits)\n"
                  "{\n"
                  "    return float2(asfloat(bits << 16u),\n"
                  "                  asfloat(bits & 0xffff0000u));\n"
                  "}\n\n",
                  "vec2 eacpUnpackBFloat16x2(uint bits)\n"
                  "{\n"
                  "    return vec2(uintBitsToFloat(bits << 16u),\n"
                  "                uintBitsToFloat(bits & 0xffff0000u));\n"
                  "}\n\n"},

    // One element chosen by a parity, as eacpReadHalf does: shifting the wanted
    // half down and back up leaves it at the top of the word with zeroes under
    // it, which is the widened float already.
    ShaderHelper {"eacpReadBFloat16",
                  "inline float eacpReadBFloat16(uint bits, uint parity)\n"
                  "{\n"
                  "    return as_type<float>((bits >> (16u * parity)) << 16u);\n"
                  "}\n\n",
                  "float eacpReadBFloat16(uint bits, uint parity)\n"
                  "{\n"
                  "    return asfloat((bits >> (16u * parity)) << 16u);\n"
                  "}\n\n",
                  "float eacpReadBFloat16(uint bits, uint parity)\n"
                  "{\n"
                  "    return uintBitsToFloat((bits >> (16u * parity)) << 16u);\n"
                  "}\n\n"},

    ShaderHelper {"eacpPackBFloat16x2",
                  packBFloat16HelperMetal,
                  packBFloat16HelperHlsl,
                  packBFloat16HelperGlsl},

    // One byte chosen by its position in the word, as eacpReadHalf and
    // eacpReadBFloat16 choose a half: shifting the wanted byte down is one
    // instruction everywhere, where selecting between four unpacked values
    // computes four and keeps one.
    ShaderHelper {"eacpReadInt8",
                  "float eacpReadInt8(uint bits, uint byteIndex)\n"
                  "{\n"
                  "    uint value = (bits >> (8u * byteIndex)) & 0xffu;\n"
                  "    return float(value ^ 0x80u) - 128.0;\n"
                  "}\n\n",
                  "float eacpReadInt8(uint bits, uint byteIndex)\n"
                  "{\n"
                  "    uint value = (bits >> (8u * byteIndex)) & 0xffu;\n"
                  "    return float(value ^ 0x80u) - 128.0;\n"
                  "}\n\n",
                  "float eacpReadInt8(uint bits, uint byteIndex)\n"
                  "{\n"
                  "    uint value = (bits >> (8u * byteIndex)) & 0xffu;\n"
                  "    return float(value ^ 0x80u) - 128.0;\n"
                  "}\n\n"},

    ShaderHelper {"eacpReadUInt8",
                  "float eacpReadUInt8(uint bits, uint byteIndex)\n"
                  "{\n"
                  "    return float((bits >> (8u * byteIndex)) & 0xffu);\n"
                  "}\n\n",
                  "float eacpReadUInt8(uint bits, uint byteIndex)\n"
                  "{\n"
                  "    return float((bits >> (8u * byteIndex)) & 0xffu);\n"
                  "}\n\n",
                  "float eacpReadUInt8(uint bits, uint byteIndex)\n"
                  "{\n"
                  "    return float((bits >> (8u * byteIndex)) & 0xffu);\n"
                  "}\n\n"},

    ShaderHelper {"eacpUnpackInt8x4",
                  unpackInt8x4Helper,
                  unpackInt8x4Helper,
                  unpackInt8x4HelperGlsl},

    ShaderHelper {"eacpUnpackUInt8x4",
                  unpackUInt8x4Helper,
                  unpackUInt8x4Helper,
                  unpackUInt8x4HelperGlsl},

    ShaderHelper {"eacpUnpackInt4x4",
                  unpackInt4x4Helper,
                  unpackInt4x4Helper,
                  unpackInt4x4HelperGlsl},

    ShaderHelper {"eacpUnpackUInt4x4",
                  unpackUInt4x4Helper,
                  unpackUInt4x4Helper,
                  unpackUInt4x4HelperGlsl},

    ShaderHelper {
        "eacpPackInt8x4", packInt8x4Helper, packInt8x4Helper, packInt8x4HelperGlsl},

    ShaderHelper {"eacpPackUInt8x4",
                  packUInt8x4Helper,
                  packUInt8x4Helper,
                  packUInt8x4HelperGlsl},

    ShaderHelper {"log10", nullptr, nullptr, log10HelperGlsl}};

const char* helperDefinition(const ShaderHelper& helper, Backend backend)
{
    switch (backend)
    {
        case Backend::Metal:
            return helper.metal;
        case Backend::DirectX:
            return helper.directX;
        case Backend::Vulkan:
            return helper.glsl;
    }

    return helper.metal;
}

// The one helper a graph needs that no expression node names: the fallback's
// packed fragment load is a statement, and it widens each of a lane's two
// elements through the same helper a scalar packed read goes through. Metal
// loads such a patch as a packed fragment and calls nothing.
bool helperWidensPackedSimdMatrix(const ShaderGraph& graph,
                                  std::string_view name,
                                  Backend backend)
{
    if (backend == Backend::Metal)
        return false;

    if (name == "eacpReadHalf")
        return graph.usesPackedSimdMatrix(SimdMatrixElement::Half);

    return name == "eacpReadBFloat16"
           && graph.usesPackedSimdMatrix(SimdMatrixElement::BFloat16);
}

bool graphCallsHelper(const ShaderGraph& graph, std::string_view name)
{
    for (auto node = 0; node < graph.nodeCount(); ++node)
    {
        const auto& expr = graph.expr(node);

        if (expr.kind == ExprKind::Call && expr.text == name)
            return true;
    }

    return false;
}

bool isHelperUsed(const ShaderGraph& graph, std::string_view name, Backend backend)
{
    return helperWidensPackedSimdMatrix(graph, name, backend)
           || graphCallsHelper(graph, name);
}
} // namespace

// Only the helpers a graph actually calls, so a shader that unpacks nothing
// carries no definition for one.
std::string helperDefinitions(const ShaderGraph& graph, Backend backend)
{
    auto definitions = std::string {};

    for (const auto& helper: shaderHelpers)
    {
        if (!isHelperUsed(graph, helper.name, backend))
            continue;

        if (const auto* definition = helperDefinition(helper, backend))
            definitions += definition;
    }

    return definitions;
}

std::string sourcePreamble(const ShaderGraph& graph, Backend backend)
{
    auto source = std::string {};

    if (backend == Backend::Metal)
        source += "#include <metal_stdlib>\nusing namespace metal;\n\n";

    if (backend == Backend::Vulkan)
        source += "#version 450\n\n";

    return source + helperDefinitions(graph, backend);
}
} // namespace eacp::GPU::ShaderEmission

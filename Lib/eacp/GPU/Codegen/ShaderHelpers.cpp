#include "ShaderHelpers.h"

#include "ShaderOverrideEmission.h"
#include "ShaderStructEmission.h"

namespace eacp::GPU::ShaderEmission
{
namespace
{

// A builtin the dialects cannot spell as one renamed call. Each definition
// stands alone: which ones are emitted is decided per helper.
struct ShaderHelper
{
    std::string_view name;

    // Null where the dialect has the function natively.
    std::string_view metal;
    std::string_view directX;
    std::string_view glsl;
};

// Abramowitz & Stegun 7.1.26; neither MSL nor HLSL has erf. Worst absolute
// error under 6e-7, pinned by Tests/GPU/IntrinsicTests.
constexpr auto erfHelper = std::string_view {
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
    "}\n\n"};

// Computed directly rather than as 1 - erf(x), which is zero for the whole
// tail in float32.
constexpr auto erfcHelper = std::string_view {
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
    "}\n\n"};

constexpr auto erfHelperGlsl = std::string_view {
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
    "}\n\n"};

constexpr auto erfcHelperGlsl = std::string_view {
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
    "}\n\n"};

// Under fast math the native tanh overflows through exp past 44 and returns
// NaN. Past ten float32 tanh is exactly one, so the branch changes no value.
constexpr auto saturatingTanhHelper = std::string_view {
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
    "}\n\n"};

constexpr auto saturatingTanhHelperGlsl = std::string_view {
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
    "}\n\n"};

// log2 scaled by log10(2), which is what a driver's own log10 lowers to.
constexpr auto log10HelperGlsl =
    std::string_view {"float eacpLog10(float x)\n"
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
                      "}\n\n"};

// Round-to-nearest-even in integer arithmetic so every backend agrees to the
// bit. A NaN is quieted first: rounding one can carry into an infinity.
constexpr auto packBFloat16HelperMetal = std::string_view {
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
    "}\n\n"};

constexpr auto packBFloat16HelperHlsl = std::string_view {
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
    "}\n\n"};

constexpr auto packBFloat16HelperGlsl = std::string_view {
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
    "}\n\n"};

// (b ^ 0x80) - 128 sign-extends identically in every dialect, unlike a shift
// through the sign bit.
constexpr auto unpackInt8x4Helper = std::string_view {
    "float4 eacpUnpackInt8x4(uint bits)\n"
    "{\n"
    "    return float4(float((bits & 0xffu) ^ 0x80u) - 128.0,\n"
    "                  float(((bits >> 8u) & 0xffu) ^ 0x80u) - 128.0,\n"
    "                  float(((bits >> 16u) & 0xffu) ^ 0x80u) - 128.0,\n"
    "                  float(((bits >> 24u) & 0xffu) ^ 0x80u) - 128.0);\n"
    "}\n\n"};

constexpr auto unpackInt8x4HelperGlsl = std::string_view {
    "vec4 eacpUnpackInt8x4(uint bits)\n"
    "{\n"
    "    return vec4(float((bits & 0xffu) ^ 0x80u) - 128.0,\n"
    "                float(((bits >> 8u) & 0xffu) ^ 0x80u) - 128.0,\n"
    "                float(((bits >> 16u) & 0xffu) ^ 0x80u) - 128.0,\n"
    "                float(((bits >> 24u) & 0xffu) ^ 0x80u) - 128.0);\n"
    "}\n\n"};

constexpr auto unpackUInt8x4Helper =
    std::string_view {"float4 eacpUnpackUInt8x4(uint bits)\n"
                      "{\n"
                      "    return float4(float(bits & 0xffu),\n"
                      "                  float((bits >> 8u) & 0xffu),\n"
                      "                  float((bits >> 16u) & 0xffu),\n"
                      "                  float((bits >> 24u) & 0xffu));\n"
                      "}\n\n"};

constexpr auto unpackUInt8x4HelperGlsl =
    std::string_view {"vec4 eacpUnpackUInt8x4(uint bits)\n"
                      "{\n"
                      "    return vec4(float(bits & 0xffu),\n"
                      "                float((bits >> 8u) & 0xffu),\n"
                      "                float((bits >> 16u) & 0xffu),\n"
                      "                float((bits >> 24u) & 0xffu));\n"
                      "}\n\n"};

// The high four nibbles are this applied to the word shifted down sixteen.
constexpr auto unpackInt4x4Helper = std::string_view {
    "float4 eacpUnpackInt4x4(uint bits)\n"
    "{\n"
    "    return float4(float((bits & 0xfu) ^ 0x8u) - 8.0,\n"
    "                  float(((bits >> 4u) & 0xfu) ^ 0x8u) - 8.0,\n"
    "                  float(((bits >> 8u) & 0xfu) ^ 0x8u) - 8.0,\n"
    "                  float(((bits >> 12u) & 0xfu) ^ 0x8u) - 8.0);\n"
    "}\n\n"};

constexpr auto unpackInt4x4HelperGlsl = std::string_view {
    "vec4 eacpUnpackInt4x4(uint bits)\n"
    "{\n"
    "    return vec4(float((bits & 0xfu) ^ 0x8u) - 8.0,\n"
    "                float(((bits >> 4u) & 0xfu) ^ 0x8u) - 8.0,\n"
    "                float(((bits >> 8u) & 0xfu) ^ 0x8u) - 8.0,\n"
    "                float(((bits >> 12u) & 0xfu) ^ 0x8u) - 8.0);\n"
    "}\n\n"};

constexpr auto unpackUInt4x4Helper =
    std::string_view {"float4 eacpUnpackUInt4x4(uint bits)\n"
                      "{\n"
                      "    return float4(float(bits & 0xfu),\n"
                      "                  float((bits >> 4u) & 0xfu),\n"
                      "                  float((bits >> 8u) & 0xfu),\n"
                      "                  float((bits >> 12u) & 0xfu));\n"
                      "}\n\n"};

constexpr auto unpackUInt4x4HelperGlsl =
    std::string_view {"vec4 eacpUnpackUInt4x4(uint bits)\n"
                      "{\n"
                      "    return vec4(float(bits & 0xfu),\n"
                      "                float((bits >> 4u) & 0xfu),\n"
                      "                float((bits >> 8u) & 0xfu),\n"
                      "                float((bits >> 12u) & 0xfu));\n"
                      "}\n\n"};

// Masked in the signed domain first, so the uint conversion never sees a
// negative value, which the dialects convert differently.
constexpr auto packInt8x4Helper = std::string_view {
    "uint eacpPackInt8x4(int4 values)\n"
    "{\n"
    "    return uint(values.x & 0xff) | (uint(values.y & 0xff) << 8u)\n"
    "           | (uint(values.z & 0xff) << 16u)\n"
    "           | (uint(values.w & 0xff) << 24u);\n"
    "}\n\n"};

constexpr auto packInt8x4HelperGlsl = std::string_view {
    "uint eacpPackInt8x4(ivec4 values)\n"
    "{\n"
    "    return uint(values.x & 0xff) | (uint(values.y & 0xff) << 8u)\n"
    "           | (uint(values.z & 0xff) << 16u)\n"
    "           | (uint(values.w & 0xff) << 24u);\n"
    "}\n\n"};

constexpr auto packUInt8x4Helper =
    std::string_view {"uint eacpPackUInt8x4(uint4 values)\n"
                      "{\n"
                      "    return (values.x & 0xffu) | ((values.y & 0xffu) << 8u)\n"
                      "           | ((values.z & 0xffu) << 16u)\n"
                      "           | ((values.w & 0xffu) << 24u);\n"
                      "}\n\n"};

constexpr auto packUInt8x4HelperGlsl =
    std::string_view {"uint eacpPackUInt8x4(uvec4 values)\n"
                      "{\n"
                      "    return (values.x & 0xffu) | ((values.y & 0xffu) << 8u)\n"
                      "           | ((values.z & 0xffu) << 16u)\n"
                      "           | ((values.w & 0xffu) << 24u);\n"
                      "}\n\n"};

// WGSL defines these three's accuracy by the logarithm forms; HLSL has none of
// them. asinh takes x's sign after the fact, so the sum never cancels.
constexpr auto asinhHelperHlsl = std::string_view {
    "float eacpAsinh(float x)\n"
    "{\n"
    "    float a = abs(x);\n"
    "    return asfloat(asuint(log(a + sqrt(a * a + 1.0))) | (asuint(x) & "
    "0x80000000u));\n"
    "}\n\n"
    "float2 eacpAsinh(float2 x)\n"
    "{\n"
    "    float2 a = abs(x);\n"
    "    return asfloat(asuint(log(a + sqrt(a * a + 1.0))) | (asuint(x) & "
    "0x80000000u));\n"
    "}\n\n"
    "float3 eacpAsinh(float3 x)\n"
    "{\n"
    "    float3 a = abs(x);\n"
    "    return asfloat(asuint(log(a + sqrt(a * a + 1.0))) | (asuint(x) & "
    "0x80000000u));\n"
    "}\n\n"
    "float4 eacpAsinh(float4 x)\n"
    "{\n"
    "    float4 a = abs(x);\n"
    "    return asfloat(asuint(log(a + sqrt(a * a + 1.0))) | (asuint(x) & "
    "0x80000000u));\n"
    "}\n\n"};

constexpr auto asinhHelperMetal =
    std::string_view {"float eacpAsinh(float x)\n"
                      "{\n"
                      "    return copysign(asinh(abs(x)), x);\n"
                      "}\n\n"
                      "float2 eacpAsinh(float2 x)\n"
                      "{\n"
                      "    return copysign(asinh(abs(x)), x);\n"
                      "}\n\n"
                      "float3 eacpAsinh(float3 x)\n"
                      "{\n"
                      "    return copysign(asinh(abs(x)), x);\n"
                      "}\n\n"
                      "float4 eacpAsinh(float4 x)\n"
                      "{\n"
                      "    return copysign(asinh(abs(x)), x);\n"
                      "}\n\n"};

constexpr auto acoshHelperHlsl =
    std::string_view {"float eacpAcosh(float x)\n"
                      "{\n"
                      "    return log(x + sqrt(x * x - 1.0));\n"
                      "}\n\n"
                      "float2 eacpAcosh(float2 x)\n"
                      "{\n"
                      "    return log(x + sqrt(x * x - 1.0));\n"
                      "}\n\n"
                      "float3 eacpAcosh(float3 x)\n"
                      "{\n"
                      "    return log(x + sqrt(x * x - 1.0));\n"
                      "}\n\n"
                      "float4 eacpAcosh(float4 x)\n"
                      "{\n"
                      "    return log(x + sqrt(x * x - 1.0));\n"
                      "}\n\n"};

constexpr auto atanhHelperHlsl =
    std::string_view {"float eacpAtanh(float x)\n"
                      "{\n"
                      "    return 0.5 * log((1.0 + x) / (1.0 - x));\n"
                      "}\n\n"
                      "float2 eacpAtanh(float2 x)\n"
                      "{\n"
                      "    return 0.5 * log((1.0 + x) / (1.0 - x));\n"
                      "}\n\n"
                      "float3 eacpAtanh(float3 x)\n"
                      "{\n"
                      "    return 0.5 * log((1.0 + x) / (1.0 - x));\n"
                      "}\n\n"
                      "float4 eacpAtanh(float4 x)\n"
                      "{\n"
                      "    return 0.5 * log((1.0 + x) / (1.0 - x));\n"
                      "}\n\n"};

constexpr auto quantizeToF16HelperMetal =
    std::string_view {"float eacpQuantizeToF16(float x)\n"
                      "{\n"
                      "    return float(half(x));\n"
                      "}\n\n"
                      "float2 eacpQuantizeToF16(float2 x)\n"
                      "{\n"
                      "    return float2(half2(x));\n"
                      "}\n\n"
                      "float3 eacpQuantizeToF16(float3 x)\n"
                      "{\n"
                      "    return float3(half3(x));\n"
                      "}\n\n"
                      "float4 eacpQuantizeToF16(float4 x)\n"
                      "{\n"
                      "    return float4(half4(x));\n"
                      "}\n\n"};

constexpr auto quantizeToF16HelperHlsl =
    std::string_view {"float eacpQuantizeToF16(float x)\n"
                      "{\n"
                      "    return f16tof32(f32tof16(x));\n"
                      "}\n\n"
                      "float2 eacpQuantizeToF16(float2 x)\n"
                      "{\n"
                      "    return f16tof32(f32tof16(x));\n"
                      "}\n\n"
                      "float3 eacpQuantizeToF16(float3 x)\n"
                      "{\n"
                      "    return f16tof32(f32tof16(x));\n"
                      "}\n\n"
                      "float4 eacpQuantizeToF16(float4 x)\n"
                      "{\n"
                      "    return f16tof32(f32tof16(x));\n"
                      "}\n\n"};

constexpr auto quantizeToF16HelperGlsl = std::string_view {
    "float eacpQuantizeToF16(float x)\n"
    "{\n"
    "    uint bits = floatBitsToUint(x);\n"
    "    uint sign = bits & 0x80000000u;\n"
    "    uint magnitude = bits & 0x7fffffffu;\n"
    "    if (magnitude > 0x7f800000u)\n"
    "        return uintBitsToFloat(sign | 0x7fc00000u | (magnitude & "
    "0x7fe000u));\n"
    "    if (magnitude >= 0x477ff000u)\n"
    "        return uintBitsToFloat(sign | 0x7f800000u);\n"
    "    if (magnitude >= 0x38800000u)\n"
    "    {\n"
    "        uint rounded = magnitude + 0xfffu + ((magnitude >> 13u) & 1u);\n"
    "        return uintBitsToFloat(sign | (rounded & 0xffffe000u));\n"
    "    }\n"
    "    float steps = roundEven(uintBitsToFloat(magnitude) * 16777216.0);\n"
    "    return uintBitsToFloat(sign | floatBitsToUint(steps * "
    "5.9604644775390625e-8));\n"
    "}\n\n"
    "vec2 eacpQuantizeToF16(vec2 x)\n"
    "{\n"
    "    return vec2(eacpQuantizeToF16(x.x), eacpQuantizeToF16(x.y));\n"
    "}\n\n"
    "vec3 eacpQuantizeToF16(vec3 x)\n"
    "{\n"
    "    return vec3(eacpQuantizeToF16(x.xy), eacpQuantizeToF16(x.z));\n"
    "}\n\n"
    "vec4 eacpQuantizeToF16(vec4 x)\n"
    "{\n"
    "    return vec4(eacpQuantizeToF16(x.xy), eacpQuantizeToF16(x.zw));\n"
    "}\n\n"};

// WGSL's floor(0.5 + scaled), which rounds a tie up where the dialects' own
// packs round it to even or leave it to the driver.
constexpr auto packUnorm8x4HelperMetal = std::string_view {
    "uint eacpPackUnorm8x4(float4 values)\n"
    "{\n"
    "    uint4 bytes = uint4(floor(0.5 + 255.0 * clamp(values, float4(0.0), "
    "float4(1.0))));\n"
    "    return bytes.x | (bytes.y << 8u) | (bytes.z << 16u) | (bytes.w << 24u);\n"
    "}\n\n"};

constexpr auto packUnorm8x4HelperHlsl = std::string_view {
    "uint eacpPackUnorm8x4(float4 values)\n"
    "{\n"
    "    uint4 bytes = uint4(floor(0.5 + 255.0 * saturate(values)));\n"
    "    return bytes.x | (bytes.y << 8u) | (bytes.z << 16u) | (bytes.w << 24u);\n"
    "}\n\n"};

constexpr auto packUnorm8x4HelperGlsl = std::string_view {
    "uint eacpPackUnorm8x4(vec4 values)\n"
    "{\n"
    "    uvec4 bytes = uvec4(floor(0.5 + 255.0 * clamp(values, 0.0, 1.0)));\n"
    "    return bytes.x | (bytes.y << 8u) | (bytes.z << 16u) | (bytes.w << 24u);\n"
    "}\n\n"};

constexpr auto packSnorm8x4HelperMetal = std::string_view {
    "uint eacpPackSnorm8x4(float4 values)\n"
    "{\n"
    "    int4 bytes = int4(floor(0.5 + 127.0 * clamp(values, float4(-1.0), "
    "float4(1.0)))) & 0xff;\n"
    "    return uint(bytes.x) | (uint(bytes.y) << 8u) | (uint(bytes.z) << 16u)\n"
    "           | (uint(bytes.w) << 24u);\n"
    "}\n\n"};

constexpr auto packSnorm8x4HelperHlsl = std::string_view {
    "uint eacpPackSnorm8x4(float4 values)\n"
    "{\n"
    "    int4 bytes = int4(floor(0.5 + 127.0 * clamp(values, -1.0, 1.0))) & 0xff;\n"
    "    return uint(bytes.x) | (uint(bytes.y) << 8u) | (uint(bytes.z) << 16u)\n"
    "           | (uint(bytes.w) << 24u);\n"
    "}\n\n"};

constexpr auto packSnorm8x4HelperGlsl = std::string_view {
    "uint eacpPackSnorm8x4(vec4 values)\n"
    "{\n"
    "    ivec4 bytes = ivec4(floor(0.5 + 127.0 * clamp(values, -1.0, 1.0))) & 0xff;\n"
    "    return uint(bytes.x) | (uint(bytes.y) << 8u) | (uint(bytes.z) << 16u)\n"
    "           | (uint(bytes.w) << 24u);\n"
    "}\n\n"};

constexpr auto packUnorm16x2HelperMetal = std::string_view {
    "uint eacpPackUnorm16x2(float2 values)\n"
    "{\n"
    "    uint2 halves = uint2(floor(0.5 + 65535.0 * clamp(values, float2(0.0), "
    "float2(1.0))));\n"
    "    return halves.x | (halves.y << 16u);\n"
    "}\n\n"};

constexpr auto packUnorm16x2HelperHlsl = std::string_view {
    "uint eacpPackUnorm16x2(float2 values)\n"
    "{\n"
    "    uint2 halves = uint2(floor(0.5 + 65535.0 * saturate(values)));\n"
    "    return halves.x | (halves.y << 16u);\n"
    "}\n\n"};

constexpr auto packUnorm16x2HelperGlsl = std::string_view {
    "uint eacpPackUnorm16x2(vec2 values)\n"
    "{\n"
    "    uvec2 halves = uvec2(floor(0.5 + 65535.0 * clamp(values, 0.0, 1.0)));\n"
    "    return halves.x | (halves.y << 16u);\n"
    "}\n\n"};

constexpr auto packSnorm16x2HelperMetal = std::string_view {
    "uint eacpPackSnorm16x2(float2 values)\n"
    "{\n"
    "    int2 halves = int2(floor(0.5 + 32767.0 * clamp(values, float2(-1.0), "
    "float2(1.0)))) & 0xffff;\n"
    "    return uint(halves.x) | (uint(halves.y) << 16u);\n"
    "}\n\n"};

constexpr auto packSnorm16x2HelperHlsl = std::string_view {
    "uint eacpPackSnorm16x2(float2 values)\n"
    "{\n"
    "    int2 halves = int2(floor(0.5 + 32767.0 * clamp(values, -1.0, 1.0))) & "
    "0xffff;\n"
    "    return uint(halves.x) | (uint(halves.y) << 16u);\n"
    "}\n\n"};

constexpr auto packSnorm16x2HelperGlsl = std::string_view {
    "uint eacpPackSnorm16x2(vec2 values)\n"
    "{\n"
    "    ivec2 halves = ivec2(floor(0.5 + 32767.0 * clamp(values, -1.0, 1.0))) & "
    "0xffff;\n"
    "    return uint(halves.x) | (uint(halves.y) << 16u);\n"
    "}\n\n"};

constexpr auto unpackUnorm8x4Helper =
    std::string_view {"float4 eacpUnpackUnorm8x4(uint bits)\n"
                      "{\n"
                      "    return float4(float(bits & 0xffu),\n"
                      "                  float((bits >> 8u) & 0xffu),\n"
                      "                  float((bits >> 16u) & 0xffu),\n"
                      "                  float(bits >> 24u)) / 255.0;\n"
                      "}\n\n"};

constexpr auto unpackUnorm8x4HelperGlsl =
    std::string_view {"vec4 eacpUnpackUnorm8x4(uint bits)\n"
                      "{\n"
                      "    return vec4(float(bits & 0xffu),\n"
                      "                float((bits >> 8u) & 0xffu),\n"
                      "                float((bits >> 16u) & 0xffu),\n"
                      "                float(bits >> 24u)) / 255.0;\n"
                      "}\n\n"};

constexpr auto unpackSnorm8x4HelperMetal = std::string_view {
    "float4 eacpUnpackSnorm8x4(uint bits)\n"
    "{\n"
    "    float4 bytes = float4(float((bits & 0xffu) ^ 0x80u) - 128.0,\n"
    "                          float(((bits >> 8u) & 0xffu) ^ 0x80u) - 128.0,\n"
    "                          float(((bits >> 16u) & 0xffu) ^ 0x80u) - 128.0,\n"
    "                          float((bits >> 24u) ^ 0x80u) - 128.0);\n"
    "    return max(bytes / 127.0, float4(-1.0));\n"
    "}\n\n"};

constexpr auto unpackSnorm8x4HelperHlsl = std::string_view {
    "float4 eacpUnpackSnorm8x4(uint bits)\n"
    "{\n"
    "    float4 bytes = float4(float((bits & 0xffu) ^ 0x80u) - 128.0,\n"
    "                          float(((bits >> 8u) & 0xffu) ^ 0x80u) - 128.0,\n"
    "                          float(((bits >> 16u) & 0xffu) ^ 0x80u) - 128.0,\n"
    "                          float((bits >> 24u) ^ 0x80u) - 128.0);\n"
    "    return max(bytes / 127.0, -1.0);\n"
    "}\n\n"};

constexpr auto unpackSnorm8x4HelperGlsl = std::string_view {
    "vec4 eacpUnpackSnorm8x4(uint bits)\n"
    "{\n"
    "    vec4 bytes = vec4(float((bits & 0xffu) ^ 0x80u) - 128.0,\n"
    "                      float(((bits >> 8u) & 0xffu) ^ 0x80u) - 128.0,\n"
    "                      float(((bits >> 16u) & 0xffu) ^ 0x80u) - 128.0,\n"
    "                      float((bits >> 24u) ^ 0x80u) - 128.0);\n"
    "    return max(bytes / 127.0, -1.0);\n"
    "}\n\n"};

constexpr auto unpackUnorm16x2Helper = std::string_view {
    "float2 eacpUnpackUnorm16x2(uint bits)\n"
    "{\n"
    "    return float2(float(bits & 0xffffu), float(bits >> 16u))\n"
    "           / 65535.0;\n"
    "}\n\n"};

constexpr auto unpackUnorm16x2HelperGlsl =
    std::string_view {"vec2 eacpUnpackUnorm16x2(uint bits)\n"
                      "{\n"
                      "    return vec2(float(bits & 0xffffu), float(bits >> 16u))\n"
                      "           / 65535.0;\n"
                      "}\n\n"};

constexpr auto unpackSnorm16x2HelperMetal = std::string_view {
    "float2 eacpUnpackSnorm16x2(uint bits)\n"
    "{\n"
    "    float2 halves = float2(float((bits & 0xffffu) ^ 0x8000u) - 32768.0,\n"
    "                           float((bits >> 16u) ^ 0x8000u) - 32768.0);\n"
    "    return max(halves / 32767.0, float2(-1.0));\n"
    "}\n\n"};

constexpr auto unpackSnorm16x2HelperHlsl = std::string_view {
    "float2 eacpUnpackSnorm16x2(uint bits)\n"
    "{\n"
    "    float2 halves = float2(float((bits & 0xffffu) ^ 0x8000u) - 32768.0,\n"
    "                           float((bits >> 16u) ^ 0x8000u) - 32768.0);\n"
    "    return max(halves / 32767.0, -1.0);\n"
    "}\n\n"};

constexpr auto unpackSnorm16x2HelperGlsl = std::string_view {
    "vec2 eacpUnpackSnorm16x2(uint bits)\n"
    "{\n"
    "    vec2 halves = vec2(float((bits & 0xffffu) ^ 0x8000u) - 32768.0,\n"
    "                       float((bits >> 16u) ^ 0x8000u) - 32768.0);\n"
    "    return max(halves / 32767.0, -1.0);\n"
    "}\n\n"};

const auto shaderHelpers = Array<ShaderHelper, 30> {
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

    // f32tof16 zeroes the upper sixteen bits, so the low half needs no mask.
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

    // Not through fp16: its five exponent bits would flush small weights.
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

    ShaderHelper {"log10", {}, {}, log10HelperGlsl},

    ShaderHelper {"asinh", asinhHelperMetal, asinhHelperHlsl, {}},
    ShaderHelper {"acosh", {}, acoshHelperHlsl, {}},
    ShaderHelper {"atanh", {}, atanhHelperHlsl, {}},

    ShaderHelper {"eacpQuantizeToF16",
                  quantizeToF16HelperMetal,
                  quantizeToF16HelperHlsl,
                  quantizeToF16HelperGlsl},

    ShaderHelper {"eacpPackUnorm8x4",
                  packUnorm8x4HelperMetal,
                  packUnorm8x4HelperHlsl,
                  packUnorm8x4HelperGlsl},
    ShaderHelper {"eacpPackSnorm8x4",
                  packSnorm8x4HelperMetal,
                  packSnorm8x4HelperHlsl,
                  packSnorm8x4HelperGlsl},
    ShaderHelper {"eacpPackUnorm16x2",
                  packUnorm16x2HelperMetal,
                  packUnorm16x2HelperHlsl,
                  packUnorm16x2HelperGlsl},
    ShaderHelper {"eacpPackSnorm16x2",
                  packSnorm16x2HelperMetal,
                  packSnorm16x2HelperHlsl,
                  packSnorm16x2HelperGlsl},
    ShaderHelper {"eacpUnpackUnorm8x4",
                  unpackUnorm8x4Helper,
                  unpackUnorm8x4Helper,
                  unpackUnorm8x4HelperGlsl},
    ShaderHelper {"eacpUnpackSnorm8x4",
                  unpackSnorm8x4HelperMetal,
                  unpackSnorm8x4HelperHlsl,
                  unpackSnorm8x4HelperGlsl},
    ShaderHelper {"eacpUnpackUnorm16x2",
                  unpackUnorm16x2Helper,
                  unpackUnorm16x2Helper,
                  unpackUnorm16x2HelperGlsl},
    ShaderHelper {"eacpUnpackSnorm16x2",
                  unpackSnorm16x2HelperMetal,
                  unpackSnorm16x2HelperHlsl,
                  unpackSnorm16x2HelperGlsl}};

// The bit functions: a uint and an int overload per dialect, always both, the
// uint one first. Vector overloads are generated per width used, one call per
// component, since a scalar body is the one shape every corner case is written
// for once.
enum class BitHelperShape
{
    Unary,
    Extract,
    Insert
};

struct BitHelper
{
    std::string_view name;
    BitHelperShape shape;
    std::string_view metal;
    std::string_view directX;
    std::string_view glsl;
};

// With the count clamped to what is above the offset, no shift below reaches 32,
// which GLSL leaves undefined and HLSL masks.
constexpr auto extractBitsHelperMetal = std::string_view {
    "uint eacpExtractBits(uint x, uint offset, uint count)\n"
    "{\n"
    "    uint o = min(offset, 32u);\n"
    "    uint c = min(count, 32u - o);\n\n"
    "    if (c == 0u)\n"
    "        return 0u;\n\n"
    "    return (x >> o) & (0xffffffffu >> (32u - c));\n"
    "}\n\n"
    "int eacpExtractBits(int x, uint offset, uint count)\n"
    "{\n"
    "    uint o = min(offset, 32u);\n"
    "    uint c = min(count, 32u - o);\n\n"
    "    if (c == 0u)\n"
    "        return 0;\n\n"
    "    return as_type<int>(as_type<uint>(x) << (32u - o - c)) >> (32u - c);\n"
    "}\n\n"};

constexpr auto extractBitsHelperHlsl =
    std::string_view {"uint eacpExtractBits(uint x, uint offset, uint count)\n"
                      "{\n"
                      "    uint o = min(offset, 32u);\n"
                      "    uint c = min(count, 32u - o);\n\n"
                      "    if (c == 0u)\n"
                      "        return 0u;\n\n"
                      "    return (x >> o) & (0xffffffffu >> (32u - c));\n"
                      "}\n\n"
                      "int eacpExtractBits(int x, uint offset, uint count)\n"
                      "{\n"
                      "    uint o = min(offset, 32u);\n"
                      "    uint c = min(count, 32u - o);\n\n"
                      "    if (c == 0u)\n"
                      "        return 0;\n\n"
                      "    return asint(asuint(x) << (32u - o - c)) >> (32u - c);\n"
                      "}\n\n"};

constexpr auto extractBitsHelperGlsl =
    std::string_view {"uint eacpExtractBits(uint x, uint offset, uint count)\n"
                      "{\n"
                      "    uint o = min(offset, 32u);\n"
                      "    uint c = min(count, 32u - o);\n\n"
                      "    if (c == 0u)\n"
                      "        return 0u;\n\n"
                      "    return (x >> o) & (0xffffffffu >> (32u - c));\n"
                      "}\n\n"
                      "int eacpExtractBits(int x, uint offset, uint count)\n"
                      "{\n"
                      "    uint o = min(offset, 32u);\n"
                      "    uint c = min(count, 32u - o);\n\n"
                      "    if (c == 0u)\n"
                      "        return 0;\n\n"
                      "    return int(uint(x) << (32u - o - c)) >> (32u - c);\n"
                      "}\n\n"};

constexpr auto insertBitsHelperMetal = std::string_view {
    "uint eacpInsertBits(uint x, uint y, uint offset, uint count)\n"
    "{\n"
    "    uint o = min(offset, 32u);\n"
    "    uint c = min(count, 32u - o);\n\n"
    "    if (c == 0u)\n"
    "        return x;\n\n"
    "    uint mask = (0xffffffffu >> (32u - c)) << o;\n"
    "    return (x & ~mask) | ((y << o) & mask);\n"
    "}\n\n"
    "int eacpInsertBits(int x, int y, uint offset, uint count)\n"
    "{\n"
    "    return as_type<int>(\n"
    "        eacpInsertBits(as_type<uint>(x), as_type<uint>(y), offset, count));\n"
    "}\n\n"};

constexpr auto insertBitsHelperHlsl = std::string_view {
    "uint eacpInsertBits(uint x, uint y, uint offset, uint count)\n"
    "{\n"
    "    uint o = min(offset, 32u);\n"
    "    uint c = min(count, 32u - o);\n\n"
    "    if (c == 0u)\n"
    "        return x;\n\n"
    "    uint mask = (0xffffffffu >> (32u - c)) << o;\n"
    "    return (x & ~mask) | ((y << o) & mask);\n"
    "}\n\n"
    "int eacpInsertBits(int x, int y, uint offset, uint count)\n"
    "{\n"
    "    return asint(eacpInsertBits(asuint(x), asuint(y), offset, count));\n"
    "}\n\n"};

constexpr auto insertBitsHelperGlsl = std::string_view {
    "uint eacpInsertBits(uint x, uint y, uint offset, uint count)\n"
    "{\n"
    "    uint o = min(offset, 32u);\n"
    "    uint c = min(count, 32u - o);\n\n"
    "    if (c == 0u)\n"
    "        return x;\n\n"
    "    uint mask = (0xffffffffu >> (32u - c)) << o;\n"
    "    return (x & ~mask) | ((y << o) & mask);\n"
    "}\n\n"
    "int eacpInsertBits(int x, int y, uint offset, uint count)\n"
    "{\n"
    "    return int(eacpInsertBits(uint(x), uint(y), offset, count));\n"
    "}\n\n"};

// A zero is answered before the dialect's own first-bit search, so nothing
// depends on what that returns for one.
const auto bitHelpers = Array<BitHelper, 8> {
    BitHelper {"eacpCountOneBits",
               BitHelperShape::Unary,
               "uint eacpCountOneBits(uint x)\n"
               "{\n"
               "    return popcount(x);\n"
               "}\n\n"
               "int eacpCountOneBits(int x)\n"
               "{\n"
               "    return popcount(x);\n"
               "}\n\n",
               "uint eacpCountOneBits(uint x)\n"
               "{\n"
               "    return countbits(x);\n"
               "}\n\n"
               "int eacpCountOneBits(int x)\n"
               "{\n"
               "    return int(countbits(asuint(x)));\n"
               "}\n\n",
               "uint eacpCountOneBits(uint x)\n"
               "{\n"
               "    return uint(bitCount(x));\n"
               "}\n\n"
               "int eacpCountOneBits(int x)\n"
               "{\n"
               "    return bitCount(x);\n"
               "}\n\n"},

    BitHelper {"eacpReverseBits",
               BitHelperShape::Unary,
               "uint eacpReverseBits(uint x)\n"
               "{\n"
               "    return reverse_bits(x);\n"
               "}\n\n"
               "int eacpReverseBits(int x)\n"
               "{\n"
               "    return reverse_bits(x);\n"
               "}\n\n",
               "uint eacpReverseBits(uint x)\n"
               "{\n"
               "    return reversebits(x);\n"
               "}\n\n"
               "int eacpReverseBits(int x)\n"
               "{\n"
               "    return asint(reversebits(asuint(x)));\n"
               "}\n\n",
               "uint eacpReverseBits(uint x)\n"
               "{\n"
               "    return bitfieldReverse(x);\n"
               "}\n\n"
               "int eacpReverseBits(int x)\n"
               "{\n"
               "    return bitfieldReverse(x);\n"
               "}\n\n"},

    BitHelper {"eacpCountLeadingZeros",
               BitHelperShape::Unary,
               "uint eacpCountLeadingZeros(uint x)\n"
               "{\n"
               "    return clz(x);\n"
               "}\n\n"
               "int eacpCountLeadingZeros(int x)\n"
               "{\n"
               "    return clz(x);\n"
               "}\n\n",
               "uint eacpCountLeadingZeros(uint x)\n"
               "{\n"
               "    return x == 0u ? 32u : 31u - firstbithigh(x);\n"
               "}\n\n"
               "int eacpCountLeadingZeros(int x)\n"
               "{\n"
               "    return int(eacpCountLeadingZeros(asuint(x)));\n"
               "}\n\n",
               "uint eacpCountLeadingZeros(uint x)\n"
               "{\n"
               "    return x == 0u ? 32u : uint(31 - findMSB(x));\n"
               "}\n\n"
               "int eacpCountLeadingZeros(int x)\n"
               "{\n"
               "    return int(eacpCountLeadingZeros(uint(x)));\n"
               "}\n\n"},

    BitHelper {"eacpCountTrailingZeros",
               BitHelperShape::Unary,
               "uint eacpCountTrailingZeros(uint x)\n"
               "{\n"
               "    return ctz(x);\n"
               "}\n\n"
               "int eacpCountTrailingZeros(int x)\n"
               "{\n"
               "    return ctz(x);\n"
               "}\n\n",
               "uint eacpCountTrailingZeros(uint x)\n"
               "{\n"
               "    return x == 0u ? 32u : firstbitlow(x);\n"
               "}\n\n"
               "int eacpCountTrailingZeros(int x)\n"
               "{\n"
               "    return int(eacpCountTrailingZeros(asuint(x)));\n"
               "}\n\n",
               "uint eacpCountTrailingZeros(uint x)\n"
               "{\n"
               "    return x == 0u ? 32u : uint(findLSB(x));\n"
               "}\n\n"
               "int eacpCountTrailingZeros(int x)\n"
               "{\n"
               "    return int(eacpCountTrailingZeros(uint(x)));\n"
               "}\n\n"},

    // An Int's search is over its magnitude bits: ~x of a negative one, so 0
    // and -1 both have none.
    BitHelper {
        "eacpFirstLeadingBit",
        BitHelperShape::Unary,
        "uint eacpFirstLeadingBit(uint x)\n"
        "{\n"
        "    return x == 0u ? 0xffffffffu : 31u - clz(x);\n"
        "}\n\n"
        "int eacpFirstLeadingBit(int x)\n"
        "{\n"
        "    return as_type<int>(eacpFirstLeadingBit(as_type<uint>(x < 0 ? ~x "
        ": x)));\n"
        "}\n\n",
        "uint eacpFirstLeadingBit(uint x)\n"
        "{\n"
        "    return x == 0u ? 0xffffffffu : firstbithigh(x);\n"
        "}\n\n"
        "int eacpFirstLeadingBit(int x)\n"
        "{\n"
        "    return asint(eacpFirstLeadingBit(asuint(x < 0 ? ~x : x)));\n"
        "}\n\n",
        "uint eacpFirstLeadingBit(uint x)\n"
        "{\n"
        "    return x == 0u ? 0xffffffffu : uint(findMSB(x));\n"
        "}\n\n"
        "int eacpFirstLeadingBit(int x)\n"
        "{\n"
        "    return int(eacpFirstLeadingBit(uint(x < 0 ? ~x : x)));\n"
        "}\n\n"},

    BitHelper {"eacpFirstTrailingBit",
               BitHelperShape::Unary,
               "uint eacpFirstTrailingBit(uint x)\n"
               "{\n"
               "    return x == 0u ? 0xffffffffu : ctz(x);\n"
               "}\n\n"
               "int eacpFirstTrailingBit(int x)\n"
               "{\n"
               "    return as_type<int>(eacpFirstTrailingBit(as_type<uint>(x)));\n"
               "}\n\n",
               "uint eacpFirstTrailingBit(uint x)\n"
               "{\n"
               "    return x == 0u ? 0xffffffffu : firstbitlow(x);\n"
               "}\n\n"
               "int eacpFirstTrailingBit(int x)\n"
               "{\n"
               "    return asint(eacpFirstTrailingBit(asuint(x)));\n"
               "}\n\n",
               "uint eacpFirstTrailingBit(uint x)\n"
               "{\n"
               "    return x == 0u ? 0xffffffffu : uint(findLSB(x));\n"
               "}\n\n"
               "int eacpFirstTrailingBit(int x)\n"
               "{\n"
               "    return int(eacpFirstTrailingBit(uint(x)));\n"
               "}\n\n"},

    BitHelper {"eacpExtractBits",
               BitHelperShape::Extract,
               extractBitsHelperMetal,
               extractBitsHelperHlsl,
               extractBitsHelperGlsl},

    BitHelper {"eacpInsertBits",
               BitHelperShape::Insert,
               insertBitsHelperMetal,
               insertBitsHelperHlsl,
               insertBitsHelperGlsl}};

std::string_view getHelperDefinition(const ShaderHelper& helper, Backend backend)
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

// The fallback's packed fragment load widens through these helpers without
// a call node naming them.
bool doesHelperWidenPackedSimdMatrix(const ShaderGraph& graph,
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

bool doesGraphCallHelper(const ShaderGraph& graph, std::string_view name)
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
    return doesHelperWidenPackedSimdMatrix(graph, name, backend)
           || doesGraphCallHelper(graph, name);
}

std::string_view getBitHelperDefinition(const BitHelper& helper, Backend backend)
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

bool doesGraphCallHelperAt(const ShaderGraph& graph,
                           std::string_view name,
                           ValueType type)
{
    for (auto node = 0; node < graph.nodeCount(); ++node)
    {
        const auto& expr = graph.expr(node);

        if (expr.kind == ExprKind::Call && expr.text == name && expr.type == type)
            return true;
    }

    return false;
}

std::string getBitHelperParameters(BitHelperShape shape, std::string_view type)
{
    switch (shape)
    {
        case BitHelperShape::Unary:
            return Strings::format("{} x", type);
        case BitHelperShape::Extract:
            return Strings::format("{} x, uint offset, uint count", type);
        case BitHelperShape::Insert:
            return Strings::format("{0} x, {0} y, uint offset, uint count", type);
    }

    return {};
}

std::string getBitHelperComponentCall(const BitHelper& helper, int component)
{
    auto lane = getVectorComponentSuffix(component);

    switch (helper.shape)
    {
        case BitHelperShape::Unary:
            return Strings::format("{}(x{})", helper.name, lane);
        case BitHelperShape::Extract:
            return Strings::format("{}(x{}, offset, count)", helper.name, lane);
        case BitHelperShape::Insert:
            return Strings::format(
                "{0}(x{1}, y{1}, offset, count)", helper.name, lane);
    }

    return {};
}

std::string getBitHelperVectorOverload(const BitHelper& helper,
                                       Backend backend,
                                       ValueType type)
{
    auto typeName = getDialectTypeName(backend, type);
    auto components = std::string {};

    for (auto component = 0; component < componentCount(type); ++component)
    {
        if (component > 0)
            components += ", ";

        components += getBitHelperComponentCall(helper, component);
    }

    return Strings::format("{} {}({})\n{{\n    return {}({});\n}}\n\n",
                           typeName,
                           helper.name,
                           getBitHelperParameters(helper.shape, typeName),
                           typeName,
                           components);
}

constexpr auto bitHelperVectorTypes = Array<ValueType, 6> {ValueType::UInt2,
                                                           ValueType::UInt3,
                                                           ValueType::UInt4,
                                                           ValueType::Int2,
                                                           ValueType::Int3,
                                                           ValueType::Int4};

std::string getBitHelperDefinitions(const ShaderGraph& graph, Backend backend)
{
    auto definitions = std::string {};

    for (const auto& helper: bitHelpers)
    {
        if (!doesGraphCallHelper(graph, helper.name))
            continue;

        definitions += getBitHelperDefinition(helper, backend);

        for (auto type: bitHelperVectorTypes)
            if (doesGraphCallHelperAt(graph, helper.name, type))
                definitions += getBitHelperVectorOverload(helper, backend, type);
    }

    return definitions;
}

// Metal's asinh helper over half, for an f16 argument: Metal converts no
// vector implicitly, so the float overloads above do not take one.
std::string getMetalHalfAsinhDefinitions(const ShaderGraph& graph, Backend backend)
{
    if (backend != Backend::Metal)
        return {};

    auto definitions = std::string {};

    for (auto width = 1; width <= 4; ++width)
    {
        auto type = halfVectorOf(width);

        if (doesGraphCallHelperAt(graph, "asinh", type))
            definitions += Strings::format(
                "{0} eacpAsinh({0} x)\n{{\n    return copysign(asinh(abs(x)), "
                "x);\n}}\n\n",
                typeName(type));
    }

    return definitions;
}

bool doesGraphRoundToHalfAt(const ShaderGraph& graph, ValueType type)
{
    for (auto node = 0; node < graph.nodeCount(); ++node)
    {
        const auto& expr = graph.expr(node);

        if (expr.type == type && isRoundedToHalf(expr))
            return true;
    }

    return false;
}

std::string getRoundHalfVectorOverload(ValueType type)
{
    return Strings::format("{0} eacpRoundHalf({0} x)\n{{\n    return "
                           "f16tof32(f32tof16(x));\n}}\n\n",
                           hlslTypeName(type));
}

// HLSL holds a CxR matrix as R rows of C, each a vector f32tof16 takes.
std::string getRoundHalfMatrixOverload(ValueType type)
{
    auto rows = std::string {};

    for (auto row = 0; row < matrixRows(type); ++row)
        rows += Strings::format("{}eacpRoundHalf(x[{}])", row > 0 ? ", " : "", row);

    return Strings::format(
        "{0} eacpRoundHalf({0} x)\n{{\n    return {0}({1});\n}}\n\n",
        hlslTypeName(type),
        rows);
}

// The rounding HLSL wraps every f16 result in (isRoundedToHalf), one overload
// per type that is rounded somewhere; a matrix's leans on its rows'.
std::string getRoundHalfDefinitions(const ShaderGraph& graph, Backend backend)
{
    if (backend != Backend::DirectX)
        return {};

    auto vectors = Array<bool, 5> {};
    auto matrices = std::string {};

    for (auto raw = static_cast<int>(ValueType::Half);
         raw <= static_cast<int>(lastValueType);
         ++raw)
    {
        auto type = static_cast<ValueType>(raw);

        if (!doesGraphRoundToHalfAt(graph, type))
            continue;

        if (!isMatrix(type))
        {
            vectors[componentCount(type)] = true;
            continue;
        }

        vectors[matrixColumns(type)] = true;
        matrices += getRoundHalfMatrixOverload(type);
    }

    auto definitions = std::string {};

    for (auto width = 1; width <= 4; ++width)
        if (vectors[width])
            definitions += getRoundHalfVectorOverload(halfVectorOf(width));

    return definitions + matrices;
}

std::string getHelperDefinitions(const ShaderGraph& graph, Backend backend)
{
    auto definitions = std::string {};

    for (const auto& helper: shaderHelpers)
    {
        if (!isHelperUsed(graph, helper.name, backend))
            continue;

        definitions += getHelperDefinition(helper, backend);
    }

    return getRoundHalfDefinitions(graph, backend) + definitions
           + getMetalHalfAsinhDefinitions(graph, backend)
           + getBitHelperDefinitions(graph, backend);
}

// GLSL's f16 is an extension, and its 16-bit buffers another; both are named
// only where the shader uses them, so every other source is unchanged.
std::string getDialectHeader(const ShaderGraph& graph, Backend backend)
{
    switch (backend)
    {
        case Backend::Metal:
            return "#include <metal_stdlib>\nusing namespace metal;\n\n";
        case Backend::Vulkan:
            break;
        case Backend::DirectX:
            return {};
    }

    auto header = std::string {"#version 450\n"};

    if (graph.usesHalf())
        header +=
            "#extension GL_EXT_shader_explicit_arithmetic_types_float16 : require\n";

    if (graph.usesHalfStorage())
        header += "#extension GL_EXT_shader_16bit_storage : require\n";

    return header + "\n";
}
} // namespace

std::string getSourcePreamble(const ShaderGraph& graph, Backend backend)
{
    return getDialectHeader(graph, backend) + getOverrideDeclarations(graph, backend)
           + getStructDeclarations(graph, backend)
           + getHelperDefinitions(graph, backend)
           + getTextureQueryHelpers(graph, backend);
}
} // namespace eacp::GPU::ShaderEmission

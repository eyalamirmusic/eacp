#include "ShaderHelpers.h"

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

    ShaderHelper {"log10", {}, {}, log10HelperGlsl}};

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

std::string getHelperDefinitions(const ShaderGraph& graph, Backend backend)
{
    auto definitions = std::string {};

    for (const auto& helper: shaderHelpers)
    {
        if (!isHelperUsed(graph, helper.name, backend))
            continue;

        definitions += getHelperDefinition(helper, backend);
    }

    return definitions;
}

std::string_view getDialectHeader(Backend backend)
{
    switch (backend)
    {
        case Backend::Metal:
            return "#include <metal_stdlib>\nusing namespace metal;\n\n";
        case Backend::Vulkan:
            return "#version 450\n\n";
        case Backend::DirectX:
            break;
    }

    return {};
}
} // namespace

std::string getSourcePreamble(const ShaderGraph& graph, Backend backend)
{
    return std::string(getDialectHeader(backend))
           + getHelperDefinitions(graph, backend);
}
} // namespace eacp::GPU::ShaderEmission

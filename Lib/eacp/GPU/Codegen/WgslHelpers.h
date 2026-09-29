#pragma once

// The WGSL column of ShaderEmitter's helper table: the same functions, spelled
// for a language with no overloading, no conditional operator and no implicit
// conversions. A helper the other dialects overload per width is one function
// per width here, the width in its name (see isWgslOverloadedHelper).

namespace eacp::GPU::wgsl
{
struct Helper
{
    const char* name;
    const char* definition;
};

constexpr auto erfHelper =
    "fn eacpErf(x: f32) -> f32\n"
    "{\n"
    "    let a = abs(x);\n"
    "    let t = 1.0 / (1.0 + 0.3275911 * a);\n"
    "    let e = 1.0 - t * (0.254829592 + t * (-0.284496736 + t * (1.421413741\n"
    "            + t * (-1.453152027 + t * 1.061405429)))) * exp(-a * a);\n"
    "    return select(select(e, -e, x < 0.0), x, a == 0.0);\n"
    "}\n\n"
    "fn eacpErf2(x: vec2f) -> vec2f\n"
    "{\n"
    "    return vec2f(eacpErf(x.x), eacpErf(x.y));\n"
    "}\n\n"
    "fn eacpErf3(x: vec3f) -> vec3f\n"
    "{\n"
    "    return vec3f(eacpErf(x.x), eacpErf(x.y), eacpErf(x.z));\n"
    "}\n\n"
    "fn eacpErf4(x: vec4f) -> vec4f\n"
    "{\n"
    "    return vec4f(eacpErf(x.x), eacpErf(x.y), eacpErf(x.z), eacpErf(x.w));\n"
    "}\n\n";

constexpr auto erfcHelper =
    "fn eacpErfc(x: f32) -> f32\n"
    "{\n"
    "    let a = abs(x);\n"
    "    let t = 1.0 / (1.0 + 0.3275911 * a);\n"
    "    let e = t * (0.254829592 + t * (-0.284496736 + t * (1.421413741\n"
    "            + t * (-1.453152027 + t * 1.061405429)))) * exp(-a * a);\n"
    "    return select(select(e, 2.0 - e, x < 0.0), 1.0, a == 0.0);\n"
    "}\n\n"
    "fn eacpErfc2(x: vec2f) -> vec2f\n"
    "{\n"
    "    return vec2f(eacpErfc(x.x), eacpErfc(x.y));\n"
    "}\n\n"
    "fn eacpErfc3(x: vec3f) -> vec3f\n"
    "{\n"
    "    return vec3f(eacpErfc(x.x), eacpErfc(x.y), eacpErfc(x.z));\n"
    "}\n\n"
    "fn eacpErfc4(x: vec4f) -> vec4f\n"
    "{\n"
    "    return vec4f(eacpErfc(x.x), eacpErfc(x.y), eacpErfc(x.z), "
    "eacpErfc(x.w));\n"
    "}\n\n";

// select() evaluates both arms, so tanh is still called past the threshold;
// what it returns there is discarded rather than propagated.
constexpr auto saturatingTanhHelper =
    "fn eacpSaturatingTanh(x: f32) -> f32\n"
    "{\n"
    "    return select(select(tanh(x), -1.0, x <= -10.0), 1.0, x >= 10.0);\n"
    "}\n\n"
    "fn eacpSaturatingTanh2(x: vec2f) -> vec2f\n"
    "{\n"
    "    return vec2f(eacpSaturatingTanh(x.x), eacpSaturatingTanh(x.y));\n"
    "}\n\n"
    "fn eacpSaturatingTanh3(x: vec3f) -> vec3f\n"
    "{\n"
    "    return vec3f(eacpSaturatingTanh(x.x), eacpSaturatingTanh(x.y),\n"
    "                 eacpSaturatingTanh(x.z));\n"
    "}\n\n"
    "fn eacpSaturatingTanh4(x: vec4f) -> vec4f\n"
    "{\n"
    "    return vec4f(eacpSaturatingTanh(x.x), eacpSaturatingTanh(x.y),\n"
    "                 eacpSaturatingTanh(x.z), eacpSaturatingTanh(x.w));\n"
    "}\n\n";

constexpr auto log10Helper = "fn eacpLog10(x: f32) -> f32\n"
                             "{\n"
                             "    return log2(x) * 0.30102999566;\n"
                             "}\n\n"
                             "fn eacpLog102(x: vec2f) -> vec2f\n"
                             "{\n"
                             "    return log2(x) * 0.30102999566;\n"
                             "}\n\n"
                             "fn eacpLog103(x: vec3f) -> vec3f\n"
                             "{\n"
                             "    return log2(x) * 0.30102999566;\n"
                             "}\n\n"
                             "fn eacpLog104(x: vec4f) -> vec4f\n"
                             "{\n"
                             "    return log2(x) * 0.30102999566;\n"
                             "}\n\n";

// MSL's round: a half goes away from zero. WGSL's goes to even. x - trunc(x)
// is exact, so the comparison against a half is too.
constexpr auto roundHelper =
    "fn eacpRound(x: f32) -> f32\n"
    "{\n"
    "    let t = trunc(x);\n"
    "    return select(t, t + sign(x), abs(x - t) >= 0.5);\n"
    "}\n\n"
    "fn eacpRound2(x: vec2f) -> vec2f\n"
    "{\n"
    "    let t = trunc(x);\n"
    "    return select(t, t + sign(x), abs(x - t) >= vec2f(0.5));\n"
    "}\n\n"
    "fn eacpRound3(x: vec3f) -> vec3f\n"
    "{\n"
    "    let t = trunc(x);\n"
    "    return select(t, t + sign(x), abs(x - t) >= vec3f(0.5));\n"
    "}\n\n"
    "fn eacpRound4(x: vec4f) -> vec4f\n"
    "{\n"
    "    let t = trunc(x);\n"
    "    return select(t, t + sign(x), abs(x - t) >= vec4f(0.5));\n"
    "}\n\n";

constexpr auto packBFloat16Helper =
    "fn eacpPackBFloat16x2(values: vec2f) -> u32\n"
    "{\n"
    "    var low = bitcast<u32>(values.x);\n"
    "    var high = bitcast<u32>(values.y);\n"
    "    let lowIsNaN = (low & 0x7fffffffu) > 0x7f800000u;\n"
    "    let highIsNaN = (high & 0x7fffffffu) > 0x7f800000u;\n"
    "    low = select(low + 0x7fffu + ((low >> 16u) & 1u), low | 0x400000u,\n"
    "                 lowIsNaN);\n"
    "    high = select(high + 0x7fffu + ((high >> 16u) & 1u), high | 0x400000u,\n"
    "                  highIsNaN);\n"
    "    return (low >> 16u) | (high & 0xffff0000u);\n"
    "}\n\n";

constexpr Helper helpers[] = {
    {"eacpUnpackHalf2",
     "fn eacpUnpackHalf2(bits: u32) -> vec2f\n"
     "{\n"
     "    return unpack2x16float(bits);\n"
     "}\n\n"},
    {"eacpErf", erfHelper},
    {"eacpErfc", erfcHelper},
    {"eacpSaturatingTanh", saturatingTanhHelper},
    {"eacpReadHalf",
     "fn eacpReadHalf(bits: u32, parity: u32) -> f32\n"
     "{\n"
     "    return unpack2x16float(bits >> (16u * parity)).x;\n"
     "}\n\n"},
    {"eacpPackHalf2",
     "fn eacpPackHalf2(values: vec2f) -> u32\n"
     "{\n"
     "    return pack2x16float(values);\n"
     "}\n\n"},
    {"eacpUnpackBFloat16x2",
     "fn eacpUnpackBFloat16x2(bits: u32) -> vec2f\n"
     "{\n"
     "    return vec2f(bitcast<f32>(bits << 16u),\n"
     "                 bitcast<f32>(bits & 0xffff0000u));\n"
     "}\n\n"},
    {"eacpReadBFloat16",
     "fn eacpReadBFloat16(bits: u32, parity: u32) -> f32\n"
     "{\n"
     "    return bitcast<f32>((bits >> (16u * parity)) << 16u);\n"
     "}\n\n"},
    {"eacpPackBFloat16x2", packBFloat16Helper},
    {"eacpReadInt8",
     "fn eacpReadInt8(bits: u32, byteIndex: u32) -> f32\n"
     "{\n"
     "    let value = (bits >> (8u * byteIndex)) & 0xffu;\n"
     "    return f32(value ^ 0x80u) - 128.0;\n"
     "}\n\n"},
    {"eacpReadUInt8",
     "fn eacpReadUInt8(bits: u32, byteIndex: u32) -> f32\n"
     "{\n"
     "    return f32((bits >> (8u * byteIndex)) & 0xffu);\n"
     "}\n\n"},
    {"eacpUnpackInt8x4",
     "fn eacpUnpackInt8x4(bits: u32) -> vec4f\n"
     "{\n"
     "    return vec4f(f32((bits & 0xffu) ^ 0x80u) - 128.0,\n"
     "                 f32(((bits >> 8u) & 0xffu) ^ 0x80u) - 128.0,\n"
     "                 f32(((bits >> 16u) & 0xffu) ^ 0x80u) - 128.0,\n"
     "                 f32(((bits >> 24u) & 0xffu) ^ 0x80u) - 128.0);\n"
     "}\n\n"},
    {"eacpUnpackUInt8x4",
     "fn eacpUnpackUInt8x4(bits: u32) -> vec4f\n"
     "{\n"
     "    return vec4f(f32(bits & 0xffu),\n"
     "                 f32((bits >> 8u) & 0xffu),\n"
     "                 f32((bits >> 16u) & 0xffu),\n"
     "                 f32((bits >> 24u) & 0xffu));\n"
     "}\n\n"},
    {"eacpUnpackInt4x4",
     "fn eacpUnpackInt4x4(bits: u32) -> vec4f\n"
     "{\n"
     "    return vec4f(f32((bits & 0xfu) ^ 0x8u) - 8.0,\n"
     "                 f32(((bits >> 4u) & 0xfu) ^ 0x8u) - 8.0,\n"
     "                 f32(((bits >> 8u) & 0xfu) ^ 0x8u) - 8.0,\n"
     "                 f32(((bits >> 12u) & 0xfu) ^ 0x8u) - 8.0);\n"
     "}\n\n"},
    {"eacpUnpackUInt4x4",
     "fn eacpUnpackUInt4x4(bits: u32) -> vec4f\n"
     "{\n"
     "    return vec4f(f32(bits & 0xfu),\n"
     "                 f32((bits >> 4u) & 0xfu),\n"
     "                 f32((bits >> 8u) & 0xfu),\n"
     "                 f32((bits >> 12u) & 0xfu));\n"
     "}\n\n"},
    {"eacpPackInt8x4",
     "fn eacpPackInt8x4(values: vec4i) -> u32\n"
     "{\n"
     "    return u32(values.x & 0xff) | (u32(values.y & 0xff) << 8u)\n"
     "           | (u32(values.z & 0xff) << 16u)\n"
     "           | (u32(values.w & 0xff) << 24u);\n"
     "}\n\n"},
    {"eacpPackUInt8x4",
     "fn eacpPackUInt8x4(values: vec4u) -> u32\n"
     "{\n"
     "    return (values.x & 0xffu) | ((values.y & 0xffu) << 8u)\n"
     "           | ((values.z & 0xffu) << 16u)\n"
     "           | ((values.w & 0xffu) << 24u);\n"
     "}\n\n"},
    {"log10", log10Helper},
    {"round", roundHelper}};
} // namespace eacp::GPU::wgsl

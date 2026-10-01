#pragma once

#include <array>
#include <cstdint>

// The C++ twins of the eacp* shader helpers (ShaderEmitter.cpp's shaderHelpers
// table): the same operations in the same order, in float32 with no
// contraction, so the executor and every test reference share one definition.
// Each takes and returns what the helper does in the shader - a packed word is
// a uint32_t, a float2/float4 a std::array - and the vector forms of eacpErf, eacpErfc
// and eacpSaturatingTanh are the scalar applied per component, as there.
//
// Where the three dialects disagree, the twin follows Metal: packHalf2 rounds
// to nearest even and takes a finite value past 65504 to an infinity (D3D
// truncates and saturates instead). A shift amount is taken modulo 32, as the
// hardware does, so a parity or byte index out of range reads what a GPU reads.

namespace eacp::GPU::CpuCompute
{
using HelperFloat2 = std::array<float, 2>;
using HelperFloat4 = std::array<float, 4>;
using HelperInt4 = std::array<std::int32_t, 4>;
using HelperUInt4 = std::array<std::uint32_t, 4>;

// The IEEE binary16 conversions under packHalf2 and unpackHalf2: widening is
// exact for every pattern, narrowing rounds to nearest even with subnormal
// results (ties included, unlike GPU::halfFromFloat), overflow to infinity and
// a NaN kept a (quiet) NaN of its sign.
float widenHalf(std::uint16_t bits);
std::uint16_t narrowToHalf(float value);

// eacpErf: Abramowitz & Stegun 7.1.26, exactly as the shader spells it. Not
// named erf, which a using-directive would make ambiguous with ::erf(float).
float errorFunction(float x);

// eacpErfc: the same polynomial times exp(-x*x), not 1 - errorFunction(x).
float complementaryErrorFunction(float x);

// eacpSaturatingTanh: +-1 from |x| >= 10, std::tanh inside.
float saturatingTanh(float x);

HelperFloat2 unpackHalf2(std::uint32_t bits);
std::uint32_t packHalf2(HelperFloat2 values);
float readHalf(std::uint32_t bits, std::uint32_t parity);

HelperFloat2 unpackBFloat16x2(std::uint32_t bits);
std::uint32_t packBFloat16x2(HelperFloat2 values);
float readBFloat16(std::uint32_t bits, std::uint32_t parity);

float readInt8(std::uint32_t bits, std::uint32_t byteIndex);
float readUInt8(std::uint32_t bits, std::uint32_t byteIndex);

HelperFloat4 unpackInt8x4(std::uint32_t bits);
HelperFloat4 unpackUInt8x4(std::uint32_t bits);

// Four nibbles of the low sixteen bits; unpackInt4x8's high half is this over
// bits >> 16, as in the graph.
HelperFloat4 unpackInt4x4(std::uint32_t bits);
HelperFloat4 unpackUInt4x4(std::uint32_t bits);

// The low eight bits of each component, .x lowest: wraps, never saturates.
std::uint32_t packInt8x4(HelperInt4 values);
std::uint32_t packUInt8x4(HelperUInt4 values);
} // namespace eacp::GPU::CpuCompute

#pragma once

#include <array>
#include <cstdint>

// Host-side scalar packing: the bit layouts the GPU reads, written and read
// back on the CPU. No shader types, so texture and buffer code that only
// needs the encoding can include this without the shader EDSL.

namespace eacp::GPU
{
// float -> IEEE binary16, and back. Kept as bits rather than a native half
// because MSVC has no _Float16, and the two backends have to agree on the
// encoding regardless of what either compiler offers.
//
// Round-to-nearest-even in the normal range; ties round away from zero in the
// subnormal range, which is below 6e-5 and past anything vertex data cares
// about. Values too large for half saturate to infinity rather than wrapping.
std::uint16_t halfFromFloat(float value);
float halfToFloat(std::uint16_t bits);

// float -> bfloat16, and back: the host side of InputBuffer::readBFloat16 and
// packBFloat16x2, and what fills or checks a packed bf16 buffer before it is
// uploaded. No vertex format carries bf16 - these are here because they are the
// same kind of thing as the pair above and belong beside it.
//
// bf16 is fp32 with the low sixteen mantissa bits dropped, so widening is exact
// and is the bits back at the top of a word. Narrowing rounds to nearest even
// in integer arithmetic, which is what the shader helper does bit for bit, so a
// buffer packed here and read on any backend agrees with this. A NaN stays a
// NaN rather than carrying into the exponent and becoming an infinity.
std::uint16_t bfloat16FromFloat(float value);
float bfloat16ToFloat(std::uint16_t bits);

// The word layouts the quantized reads expect, and the way back out of one:
// the host side of InputBuffer::readInt8, readInt8x4 and readInt4x8, and what a
// loader turning a block-quantized checkpoint into a storage buffer writes.
// Here beside the float pair above for the same reason it is there - no vertex
// format carries either, and they are the same kind of thing.
//
// One word holds four bytes or eight nibbles, element zero in the low bits, so
// a row packed by walking it through these reads back on the GPU in the order
// it was written. The signed forms are two's complement - a byte over
// [-128, 127], a nibble over [-8, 7] - and an element is extracted as
// (b ^ 0x80) - 128 rather than cast, which is the arithmetic the shader helper
// does, so the two agree by construction rather than by both happening to be
// right.
//
// Only the low bits of each value are kept: something outside a nibble's range
// wraps rather than saturating, exactly as packInt8x4 does in a shader.
std::uint32_t int8x4FromBytes(const std::array<std::int8_t, 4>& values);
std::uint32_t uint8x4FromBytes(const std::array<std::uint8_t, 4>& values);
std::int8_t int8x4ToByte(std::uint32_t word, int index);
std::uint8_t uint8x4ToByte(std::uint32_t word, int index);

std::uint32_t int4x8FromNibbles(const std::array<std::int8_t, 8>& values);
std::uint32_t uint4x8FromNibbles(const std::array<std::uint8_t, 8>& values);
std::int8_t int4x8ToNibble(std::uint32_t word, int index);
std::uint8_t uint4x8ToNibble(std::uint32_t word, int index);
} // namespace eacp::GPU

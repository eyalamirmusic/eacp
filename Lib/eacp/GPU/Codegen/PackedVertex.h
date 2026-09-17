#pragma once

#include "../Pipeline/VertexLayout.h"
#include "ShaderValue.h"

#include <array>
#include <cstdint>

// CPU storage types for packed vertex attributes.
//
// vertexInput(&Vertex::field) reads the wire format off the field's C++ type: a
// float[4] member is a Float4 attribute, sixteen bytes of it. That is the right
// default and the wrong storage for most real vertex data, where a colour is
// four bytes, a UV pair is two halves and a normal is a packed triple. Unpacked
// to float everywhere, a typical mesh vertex is roughly twice the size it needs
// to be, and vertex fetch is a real cost in a scene renderer in a way it is not
// in a UI.
//
// Each type below says "this many bytes on the wire, that type in the shader":
// a ShaderValue, which is what define() sees, and a vertexFormat, which is what
// the pipeline is told. The shader body is unchanged and there is no unpacking
// in it - both backends widen the attribute as the vertex is fetched, in
// hardware, for free.
//
//     struct Vertex
//     {
//         float position[2];
//         GPU::Float16x2 uv;    // 4 bytes, Float2 in define()
//         GPU::UNorm8x4 color;  // 4 bytes, Float4 in define()
//     };
//
// Named for what they hold rather than after the VertexFormat entries, which
// keep the graphics-API spelling (UByte4Norm, Half2) that anyone reading a
// Metal or D3D12 table will recognise.

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

// Four bytes, read as 0..1 in the shader. What a vertex colour should be: this
// is the storage ImDrawVert and every mesh format already use, and expanding it
// to four floats costs twelve bytes a vertex to say nothing new.
struct UNorm8x4
{
    using ShaderValue = Float4;
    static constexpr auto vertexFormat = VertexFormat::UByte4Norm;

    static UNorm8x4 fromFloats(float x, float y, float z, float w);

    std::uint8_t values[4] {};
};

// Two and four halves. Half has ten bits of mantissa, so it holds a UV to about
// one part in a thousand - ample across a texture, and not enough for a world
// position, which is why positions stay float.
struct Float16x2
{
    using ShaderValue = Float2;
    static constexpr auto vertexFormat = VertexFormat::Half2;

    static Float16x2 from(float x, float y);

    std::uint16_t values[2] {};
};

struct Float16x4
{
    using ShaderValue = Float4;
    static constexpr auto vertexFormat = VertexFormat::Half4;

    static Float16x4 from(float x, float y, float z, float w);

    std::uint16_t values[4] {};
};

// Signed normalized shorts, read as -1..1. The usual storage for a normal or a
// tangent, where the value is a direction and the range is known.
struct SNorm16x2
{
    using ShaderValue = Float2;
    static constexpr auto vertexFormat = VertexFormat::Short2Norm;

    static SNorm16x2 from(float x, float y);

    std::int16_t values[2] {};
};

struct SNorm16x4
{
    using ShaderValue = Float4;
    static constexpr auto vertexFormat = VertexFormat::Short4Norm;

    static SNorm16x4 from(float x, float y, float z, float w);

    std::int16_t values[4] {};
};

static_assert(sizeof(UNorm8x4) == bytesPerAttribute(VertexFormat::UByte4Norm));
static_assert(sizeof(Float16x2) == bytesPerAttribute(VertexFormat::Half2));
static_assert(sizeof(Float16x4) == bytesPerAttribute(VertexFormat::Half4));
static_assert(sizeof(SNorm16x2) == bytesPerAttribute(VertexFormat::Short2Norm));
static_assert(sizeof(SNorm16x4) == bytesPerAttribute(VertexFormat::Short4Norm));
} // namespace eacp::GPU

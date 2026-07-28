#pragma once

#include "../Pipeline/VertexLayout.h"
#include "ShaderValue.h"

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

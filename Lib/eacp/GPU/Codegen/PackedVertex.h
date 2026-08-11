#pragma once

#include "../Pipeline/VertexLayout.h"
#include "ShaderValue.h"

#include <cstdint>

// CPU storage types for packed vertex attributes. Each says how many bytes it
// occupies on the wire and which ShaderValue define() sees; both backends widen
// the attribute during vertex fetch, so shader bodies need no unpacking.

namespace eacp::GPU
{
// Kept as bits rather than a native half because MSVC has no _Float16.
// Round-to-nearest-even; values too large for half saturate to infinity.
std::uint16_t halfFromFloat(float value);
float halfToFloat(std::uint16_t bits);

// Four bytes, read as 0..1 in the shader.
struct UNorm8x4
{
    using ShaderValue = Float4;
    static constexpr auto vertexFormat = VertexFormat::UByte4Norm;

    static UNorm8x4 fromFloats(float x, float y, float z, float w);

    std::uint8_t values[4] {};
};

// Ten bits of mantissa: about one part in a thousand, ample for a UV and not
// enough for a world position.
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

// Signed normalized shorts, read as -1..1 in the shader.
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

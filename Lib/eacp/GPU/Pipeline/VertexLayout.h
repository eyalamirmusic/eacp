#pragma once

#include "../Common.h"

namespace eacp::GPU
{
enum class VertexFormat
{
    Float,
    Float2,
    Float3,
    Float4,

    // Packed attributes, widened on fetch, so the shader still reads a float2 or
    // float4. No three-component packed format exists: D3D12 has no 16-bit
    // three-component vertex format, so pad to four.
    UByte4Norm, // 4 bytes -> Float4; 0..255 maps onto 0..1
    Half2, // 4 bytes -> Float2
    Half4, // 8 bytes -> Float4
    Short2Norm, // 4 bytes -> Float2; -32767..32767 maps onto -1..1
    Short4Norm // 8 bytes -> Float4
};

constexpr int bytesPerAttribute(VertexFormat format)
{
    switch (format)
    {
        case VertexFormat::Float:
            return 4;
        case VertexFormat::Float2:
            return 8;
        case VertexFormat::Float3:
            return 12;
        case VertexFormat::Float4:
            return 16;
        case VertexFormat::UByte4Norm:
            return 4;
        case VertexFormat::Half2:
            return 4;
        case VertexFormat::Half4:
            return 8;
        case VertexFormat::Short2Norm:
            return 4;
        case VertexFormat::Short4Norm:
            return 8;
    }

    return 0;
}

// A property of the slot, not the attribute: every attribute in a slot
// inherits its slot's step rate.
enum class StepRate
{
    PerVertex,
    PerInstance
};

struct VertexAttribute
{
    VertexFormat format = VertexFormat::Float3;
    int offset = 0;
    int bufferIndex = 0;
};

struct VertexBufferLayout
{
    int stride = 0;
    StepRate stepRate = StepRate::PerVertex;
};

// For a single-buffer draw set `stride` alone and leave `buffers` empty; for
// multiple slots populate `buffers` and let attributes name their `bufferIndex`.
struct VertexLayout
{
    VertexLayout& attribute(VertexFormat format, int offset, int bufferIndex = 0)
    {
        attributes.add({format, offset, bufferIndex});
        return *this;
    }

    // Grows `buffers` with defaults, so slots may be addressed out of order.
    // The explicit temporary avoids `add({})` resolving to Vector's empty
    // initializer_list overload, which adds nothing and spins the loop forever.
    VertexLayout& buffer(int bufferIndex,
                         int slotStride,
                         StepRate stepRate = StepRate::PerVertex)
    {
        while (buffers.size() <= bufferIndex)
            buffers.add(VertexBufferLayout {});
        buffers[bufferIndex] = {slotStride, stepRate};
        return *this;
    }

    Vector<VertexAttribute> attributes;

    // When empty, backends read `stride` as slot 0's, single-buffer.
    Vector<VertexBufferLayout> buffers;

    // Shorthand for slot 0's stride; ignored when `buffers` is non-empty.
    int stride = 0;
};
} // namespace eacp::GPU

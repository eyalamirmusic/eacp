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

    // Packed attributes: fewer bytes in the buffer, the same value in the
    // shader. Both backends widen these as the vertex is fetched, so a shader
    // reading one still reads a float2 or a float4 and needs no change - only
    // the storage shrinks, and with it the vertex bandwidth.
    //
    // This is how mesh data is normally stored and how glTF ships it: a vertex
    // of position, packed normal, packed tangent and half UVs is around 24
    // bytes, against 48 with everything unpacked.
    //
    // There is deliberately no three-component packed format. D3D12 has no
    // 16-bit three-component vertex format at all, so one would work on Metal
    // and fail to build a pipeline on Windows - the exact asymmetry this enum
    // exists to keep out of callers' hands. Pad to four instead.
    UByte4Norm, // 4 bytes -> Float4; 0..255 maps onto 0..1
    Half2, // 4 bytes -> Float2
    Half4, // 8 bytes -> Float4
    Short2Norm, // 4 bytes -> Float2; -32767..32767 maps onto -1..1
    Short4Norm // 8 bytes -> Float4
};

// How many bytes one attribute of this format occupies in the buffer. This is
// what a packed CPU field is checked against, so a struct member that does not
// match the format it declares is a compile error rather than geometry that
// comes out scrambled at a stride the pipeline and the struct disagree on.
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

// Whether a buffer slot's data advances once per vertex (classic vertex buffer)
// or once per instance (a "per-instance" buffer feeding drawInstanced). Set on
// VertexBufferLayout because it's a property of the slot, not the attribute -
// every attribute in a slot inherits its slot's step rate.
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

// Per-slot layout metadata: stride between consecutive elements, and how the
// slot steps through its buffer during a draw. Buffered separately because
// D3D12/Metal both configure stride + step rate per input slot, not per
// attribute.
struct VertexBufferLayout
{
    int stride = 0;
    StepRate stepRate = StepRate::PerVertex;
};

// Explicit per-vertex attribute description. Hand-written shaders declare it;
// the EDSL emits it via ShaderBuilder::build(). Either way the pipeline
// consumes this struct.
//
// For single-buffer draws (still the common case), set `stride` alone and
// leave `buffers` empty - the backends fall back to
// {VertexBufferLayout{stride, PerVertex}} at slot 0. For multi-buffer draws
// (e.g. instancing: a per-vertex geometry buffer + a per-instance data
// buffer), populate `buffers` with one entry per bound slot, and let attributes
// name their slot via `bufferIndex`.
struct VertexLayout
{
    VertexLayout& attribute(VertexFormat format, int offset, int bufferIndex = 0)
    {
        attributes.add({format, offset, bufferIndex});
        return *this;
    }

    // Configure a slot's stride and step rate. Grows `buffers` up to
    // `bufferIndex` with defaults if needed, so callers can address slots
    // out of order. The explicit VertexBufferLayout temporary (rather than
    // `add({})`) is deliberate: `add({})` resolves to Vector's initializer_list
    // overload with an empty list, and silently does nothing - the while
    // loop would then spin forever.
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

    // Explicit per-slot layout. When empty, backends treat the layout as
    // single-buffer at slot 0 using `stride` below - the pre-instancing shape.
    Vector<VertexBufferLayout> buffers;

    // Legacy shorthand for slot 0's stride. Preserved so existing callers
    // (hand-written shaders, tests) that set only `stride` keep working
    // unchanged. Ignored when `buffers` is non-empty.
    int stride = 0;
};
} // namespace eacp::GPU

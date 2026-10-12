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
    Short4Norm, // 8 bytes -> Float4

    // The rest of what a mesh format ships: every 8- and 16-bit integer
    // attribute at one, two or four components, normalised or not, a single
    // half, 10-10-10-2, and the 32-bit integers. Hand-written shaders and
    // renderers translating someone else's vertex layout (bgfx's AttribType,
    // glTF's accessor types) need them; the EDSL's packed types above stay the
    // short list.
    //
    // A Norm format is read as a float, 0..1 unsigned and -1..1 signed, exactly
    // as UByte4Norm and Short2Norm are. **A format without Norm is read as an
    // integer**, and the shader declares it as one: uint, uint2 or uint4 for the
    // unsigned formats (uvec in GLSL), int, int2 or int4 for the signed. That is
    // the rule D3D12 and Vulkan hold a pipeline to - a UINT format feeding a
    // float input is undefined there - so it is the rule here. Metal alone also
    // converts an integer format into a float input (UByte4 read as float4 is
    // 0..255), which a pipeline written for Metal may lean on and one meant for
    // three backends must not. isIntegerVertexFormat says which is which.
    //
    // Fewer components in the format than the shader declares fill in as 0 for
    // y and z and 1 for w, on every backend. Three components are still
    // absent below 32 bits, for the D3D12 reason given above: pad to four.
    UByte, // 1 byte -> uint
    UByte2, // 2 bytes -> uint2
    UByte4, // 4 bytes -> uint4; a bone index per byte
    UByteNorm, // 1 byte -> Float; 0..255 maps onto 0..1
    UByte2Norm, // 2 bytes -> Float2
    Byte, // 1 byte -> int
    Byte2, // 2 bytes -> int2
    Byte4, // 4 bytes -> int4
    ByteNorm, // 1 byte -> Float; -127..127 maps onto -1..1
    Byte2Norm, // 2 bytes -> Float2
    Byte4Norm, // 4 bytes -> Float4; a packed normal
    UShort, // 2 bytes -> uint
    UShort2, // 4 bytes -> uint2
    UShort4, // 8 bytes -> uint4
    UShortNorm, // 2 bytes -> Float; 0..65535 maps onto 0..1
    UShort2Norm, // 4 bytes -> Float2
    UShort4Norm, // 8 bytes -> Float4
    Short, // 2 bytes -> int
    Short2, // 4 bytes -> int2
    Short4, // 8 bytes -> int4
    ShortNorm, // 2 bytes -> Float
    Half, // 2 bytes -> Float

    // Four bytes holding x, y and z in ten bits each from the lowest, and w in
    // the top two: 0..1023 and 0..3 map onto 0..1. Normalised only, Metal
    // having no integer form of it; a packed normal or tangent at full sign is
    // stored biased (n * 0.5 + 0.5) and unbiased in the shader, as bgfx does.
    UInt1010102Norm, // 4 bytes -> Float4

    UInt, // 4 bytes -> uint
    UInt2, // 8 bytes -> uint2
    UInt3, // 12 bytes -> uint3
    UInt4, // 16 bytes -> uint4
    Int, // 4 bytes -> int
    Int2, // 8 bytes -> int2
    Int3, // 12 bytes -> int3
    Int4 // 16 bytes -> int4
};

// Whether the shader reads this format as an integer (uint or int) rather than
// a float. See the note above UByte.
bool isIntegerVertexFormat(VertexFormat format);

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
        case VertexFormat::UByte:
        case VertexFormat::UByteNorm:
        case VertexFormat::Byte:
        case VertexFormat::ByteNorm:
            return 1;
        case VertexFormat::UByte2:
        case VertexFormat::UByte2Norm:
        case VertexFormat::Byte2:
        case VertexFormat::Byte2Norm:
        case VertexFormat::UShort:
        case VertexFormat::UShortNorm:
        case VertexFormat::Short:
        case VertexFormat::ShortNorm:
        case VertexFormat::Half:
            return 2;
        case VertexFormat::UByte4:
        case VertexFormat::Byte4:
        case VertexFormat::Byte4Norm:
        case VertexFormat::UShort2:
        case VertexFormat::UShort2Norm:
        case VertexFormat::Short2:
        case VertexFormat::UInt1010102Norm:
        case VertexFormat::UInt:
        case VertexFormat::Int:
            return 4;
        case VertexFormat::UShort4:
        case VertexFormat::UShort4Norm:
        case VertexFormat::Short4:
        case VertexFormat::UInt2:
        case VertexFormat::Int2:
            return 8;
        case VertexFormat::UInt3:
        case VertexFormat::Int3:
            return 12;
        case VertexFormat::UInt4:
        case VertexFormat::Int4:
            return 16;
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
    VertexLayout& attribute(VertexFormat format, int offset, int bufferIndex = 0);

    // Configure a slot's stride and step rate. Grows `buffers` up to
    // `bufferIndex` with defaults if needed, so callers can address slots
    // out of order.
    VertexLayout& buffer(int bufferIndex,
                         int slotStride,
                         StepRate stepRate = StepRate::PerVertex);

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

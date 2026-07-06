#pragma once

#include <eacp/Core/Utils/Containers.h>

namespace eacp::GPU
{
enum class VertexFormat
{
    Float,
    Float2,
    Float3,
    Float4
};

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

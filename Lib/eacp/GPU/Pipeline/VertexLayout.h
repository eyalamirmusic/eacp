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

struct VertexAttribute
{
    VertexFormat format = VertexFormat::Float3;
    int offset = 0;
    int bufferIndex = 0;
};

// Explicit per-vertex attribute description. Hand-written shaders declare it;
// the future EDSL will emit it. Either way the pipeline consumes this struct.
struct VertexLayout
{
    VertexLayout& attribute(VertexFormat format, int offset, int bufferIndex = 0)
    {
        attributes.add({format, offset, bufferIndex});
        return *this;
    }

    Vector<VertexAttribute> attributes;
    int stride = 0;
};
} // namespace eacp::GPU

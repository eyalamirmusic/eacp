#include "VertexLayout.h"

namespace eacp::GPU
{
bool isIntegerVertexFormat(VertexFormat format)
{
    switch (format)
    {
        case VertexFormat::UByte:
        case VertexFormat::UByte2:
        case VertexFormat::UByte4:
        case VertexFormat::Byte:
        case VertexFormat::Byte2:
        case VertexFormat::Byte4:
        case VertexFormat::UShort:
        case VertexFormat::UShort2:
        case VertexFormat::UShort4:
        case VertexFormat::Short:
        case VertexFormat::Short2:
        case VertexFormat::Short4:
        case VertexFormat::UInt:
        case VertexFormat::UInt2:
        case VertexFormat::UInt3:
        case VertexFormat::UInt4:
        case VertexFormat::Int:
        case VertexFormat::Int2:
        case VertexFormat::Int3:
        case VertexFormat::Int4:
            return true;

        default:
            return false;
    }
}

VertexLayout&
    VertexLayout::attribute(VertexFormat format, int offset, int bufferIndex)
{
    attributes.add({format, offset, bufferIndex});
    return *this;
}

VertexLayout&
    VertexLayout::buffer(int bufferIndex, int slotStride, StepRate stepRate)
{
    // The explicit VertexBufferLayout temporary (rather than `add({})`) is
    // deliberate: `add({})` resolves to Vector's initializer_list overload with
    // an empty list, and silently does nothing - the loop would spin forever.
    while (buffers.size() <= bufferIndex)
        buffers.add(VertexBufferLayout {});
    buffers[bufferIndex] = {slotStride, stepRate};
    return *this;
}
} // namespace eacp::GPU

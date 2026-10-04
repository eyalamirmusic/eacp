#include "VertexLayout.h"

namespace eacp::GPU
{
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

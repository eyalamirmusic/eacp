#pragma once

#include "../Common.h"

namespace eacp::GPU
{
class Device;

// What a buffer is bound as. A Vertex buffer feeds the vertex stage; an Index
// buffer feeds drawIndexed; a Storage buffer is read/written by a compute
// kernel and can be read back to the CPU. On Metal all are plain MTLBuffers;
// on D3D12 the usage picks the resource flags (a Storage buffer allows
// unordered access so a kernel can write it).
enum class BufferUsage
{
    Vertex,
    Index,
    Storage
};

// The width of the indices in an Index buffer, told to drawIndexed.
enum class IndexFormat
{
    UInt16,
    UInt32
};

// RAII wrapper around a GPU buffer (MTLBuffer on Metal). Create via
// Device::makeBuffer. Pass null data with a byte count to allocate an
// uninitialised buffer (e.g. a compute output target).
class Buffer
{
public:
    Buffer(Device& device,
           const void* data,
           std::size_t bytes,
           BufferUsage usage = BufferUsage::Vertex);

    std::size_t size() const;
    bool isValid() const;

    // Copies bytes back from the buffer into dst. Valid once the command buffer
    // that wrote it has committed (CommandBuffer::commit blocks until then).
    void read(void* dst, std::size_t bytes) const;

    // Overwrites the buffer's contents from the CPU — the per-frame path for
    // dynamic geometry, reusing the GPU resource instead of allocating a new
    // one. Copies min(bytes, size()) bytes; a no-op on an invalid buffer or
    // null data. The new contents are seen by commands encoded after the
    // call; update at most once per displayed frame, as pacing against
    // frames still in flight is not synchronised here.
    void update(const void* data, std::size_t bytes);

    // Opaque native handle for cross-translation-unit use by other GPU types.
    void* nativeBuffer() const;

    // The read and write handles a compute pass binds on D3D12 (the same
    // handle as nativeBuffer; the pass binds by GPU address and direction).
    // Null on Metal, where the buffer is bound directly.
    void* nativeReadView() const;
    void* nativeWriteView() const;

private:
    struct Native;
    Pimpl<Native> impl;
};
} // namespace eacp::GPU

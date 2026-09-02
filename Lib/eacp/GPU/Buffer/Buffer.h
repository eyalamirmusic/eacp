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

    // Copies bytes back from the buffer into dst, starting at offset bytes
    // into the buffer. Valid once the command buffer that wrote it has
    // committed (CommandBuffer::commit blocks until then). The copy is
    // clamped to the buffer's end; an offset past it reads nothing.
    //
    // Committed is also the rule for bytes the CPU wrote: a construction or
    // update that happened while a Frame was recording put its copy on that
    // frame's list, so it has not reached the buffer until the frame ends.
    // Reading inside the frame that filled it reads what was there before.
    void read(void* dst, std::size_t bytes, std::size_t offset = 0) const;

    // Overwrites part of the buffer's contents from the CPU, starting at
    // offset bytes into the buffer — the per-frame path for dynamic
    // geometry, reusing the GPU resource instead of allocating a new one.
    // The copy is clamped to the buffer's end; a no-op on an invalid buffer,
    // null data or an offset past the end. The new contents are seen by
    // commands encoded after the call; update at most once per displayed
    // frame, as pacing against frames still in flight is not synchronised
    // here.
    void update(const void* data, std::size_t bytes, std::size_t offset = 0);

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

// A contiguous slice of one Buffer: where it starts and how long it is, in
// bytes from the buffer's beginning.
//
// This is what a sub-allocator hands out and what a bind can take. A
// StreamingBuffers write comes back as one of these rather than as a whole
// buffer, and RenderPass::setVertexBuffer and drawIndexed accept one directly,
// which is what lets a frame's worth of geometry share a single GPU resource
// instead of being one resource per draw. Nothing about the buffer changes:
// the range only says which part of it a draw should read from.
//
// Holds a pointer, not ownership. Whoever handed the range out owns the buffer
// and says how long the range stays valid - a streamed one until its pool
// comes round again, a range over an app's own buffer as long as the app
// keeps the buffer.
struct BufferRange
{
    const Buffer* buffer = nullptr;
    std::size_t offset = 0;
    std::size_t bytes = 0;

    // The whole of a buffer, for the calls that take a range when what a
    // caller has is a buffer it means to bind from the start.
    static BufferRange of(const Buffer& whole)
    {
        return {&whole, 0, whole.size()};
    }

    // False for a default-constructed range and for one over a buffer that
    // never got storage, which is the same test a bind makes before encoding.
    bool isValid() const { return buffer != nullptr && buffer->isValid(); }
};
} // namespace eacp::GPU

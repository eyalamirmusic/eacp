#pragma once

#include <eacp/Core/Utils/Common.h>

#include <cstddef>

namespace eacp::GPU
{
class Device;

// What a buffer is bound as. A Vertex buffer feeds the vertex stage; a Storage
// buffer is read/written by a compute kernel and can be read back to the CPU.
// On Metal both are plain MTLBuffers; on D3D11 a Storage buffer additionally
// carries the shader-resource and unordered-access views compute binds.
enum class BufferUsage
{
    Vertex,
    Storage
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

    // Opaque native handle for cross-translation-unit use by other GPU types.
    void* nativeBuffer() const;

    // The read (shader-resource) and write (unordered-access) views a compute
    // pass binds on D3D11. Null on Metal, where the buffer is bound directly.
    void* nativeReadView() const;
    void* nativeWriteView() const;

private:
    struct Native;
    Pimpl<Native> impl;
};
} // namespace eacp::GPU

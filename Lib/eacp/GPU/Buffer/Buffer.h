#pragma once

#include "../Common.h"

namespace eacp::GPU
{
class Device;

// Metal ignores this; on D3D12 it picks the resource flags (Storage allows
// unordered access so a kernel can write it).
enum class BufferUsage
{
    Vertex,
    Index,
    Storage
};

enum class IndexFormat
{
    UInt16,
    UInt32
};

// Create via Device::makeBuffer. Null data with a byte count allocates an
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

    // Clamped to the buffer's end. Only sees writes from command buffers that
    // have committed — a write issued during an open Frame lands at frame end.
    void read(void* dst, std::size_t bytes, std::size_t offset = 0) const;

    // Clamped to the buffer's end; seen by commands encoded after the call.
    // Not synchronised against frames still in flight, so call at most once
    // per displayed frame.
    void update(const void* data, std::size_t bytes, std::size_t offset = 0);

    void* nativeBuffer() const;

    // Null on Metal, where the buffer is bound directly.
    void* nativeReadView() const;
    void* nativeWriteView() const;

private:
    struct Native;
    Pimpl<Native> impl;
};
} // namespace eacp::GPU

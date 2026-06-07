#pragma once

#include <eacp/Core/Utils/Common.h>

#include <cstddef>

namespace eacp::GPU
{
class Device;

// RAII wrapper around a GPU buffer (MTLBuffer on Metal). Create via
// Device::makeBuffer.
class Buffer
{
public:
    Buffer(Device& device, const void* data, std::size_t bytes);

    std::size_t size() const;
    bool isValid() const;

    // Opaque native handle for cross-translation-unit use by other GPU types.
    void* nativeBuffer() const;

private:
    struct Native;
    Pimpl<Native> impl;
};
} // namespace eacp::GPU

#pragma once

#include "../Common.h"

#include "../Frame/ComputePass.h"

#include <eacp/Core/Threads/Async.h>

namespace eacp::GPU
{
class Device;

// Off-screen work with no drawable to present; create via
// Device::makeCommandBuffer.
class CommandBuffer
{
public:
    explicit CommandBuffer(Device& device);

    CommandBuffer(const CommandBuffer&) = delete;
    CommandBuffer& operator=(const CommandBuffer&) = delete;

    ComputePass beginCompute();

    // Blocks until the GPU finishes, after which written Storage buffers are
    // safe to read(). Committing twice is a no-op, whichever pair is used.
    void commit();

    // Submits without waiting; resolves on the main thread once the GPU is done.
    Threads::Async<void> commitAsync();

    bool isValid() const;

private:
    struct Native;
    Pimpl<Native> impl;
};
} // namespace eacp::GPU

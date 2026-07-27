#pragma once

#include "../Common.h"

#include "../Frame/ComputePass.h"

#include <eacp/Core/Threads/Async.h>

namespace eacp::GPU
{
class Device;

// An off-screen command buffer: records compute (and later, blit) work that has
// no drawable to present. The headless sibling of Frame - it owns a command
// buffer off the device queue but never touches a swapchain. commit() blocks
// until the GPU finishes, so any Storage buffer written by the pass is safe to
// read() afterwards. Create via Device::makeCommandBuffer.
class CommandBuffer
{
public:
    explicit CommandBuffer(Device& device);

    CommandBuffer(const CommandBuffer&) = delete;
    CommandBuffer& operator=(const CommandBuffer&) = delete;

    ComputePass beginCompute();

    // Submits the recorded work and waits for completion.
    void commit();

    // Submits the recorded work and returns without waiting. The returned Async
    // resolves on the main thread once the GPU has finished, at which point
    // every Storage buffer the passes wrote is safe to read().
    //
    // This is what lets the CPU carry on while the kernel runs, which is the
    // whole reason to hand work to the GPU that this frame does not need the
    // answer to. Nothing about correctness changes: a read() before the Async
    // resolves is still right, it just waits for the same work by hand and
    // gives the overlap back.
    //
    // Committing twice does nothing the second time, whichever pair is used.
    Threads::Async<void> commitAsync();

    bool isValid() const;

private:
    struct Native;
    Pimpl<Native> impl;
};
} // namespace eacp::GPU

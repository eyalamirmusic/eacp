#pragma once

#include <eacp/Core/Utils/Common.h>

#include "../Frame/ComputePass.h"

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

    bool isValid() const;

private:
    struct Native;
    Pimpl<Native> impl;
};
} // namespace eacp::GPU

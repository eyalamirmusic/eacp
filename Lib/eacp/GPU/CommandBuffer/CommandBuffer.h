#pragma once

#include "../Common.h"

#include "../Frame/ComputePass.h"
#include "../Timing/FrameTimings.h"

#include <eacp/Core/Threads/Async.h>

#include <cstdint>
#include <string_view>

namespace eacp::GPU
{
class Device;

// An off-screen command buffer: records compute and fill work that has no
// drawable to present. The headless sibling of Frame - it owns a command
// buffer off the device queue but never touches a swapchain. commit() blocks
// until the GPU finishes, so any Storage buffer written by the pass is safe to
// read() afterwards. Create via Device::makeCommandBuffer.
class CommandBuffer
{
public:
    explicit CommandBuffer(Device& device);

    CommandBuffer(const CommandBuffer&) = delete;
    CommandBuffer& operator=(const CommandBuffer&) = delete;

    // A label times the pass on the GPU and names it in timings() below. An
    // unlabelled pass is not timed and costs nothing.
    ComputePass beginCompute(std::string_view label = {});

    // Fills every byte of the range with value on the GPU, in order with the
    // passes either side of it. The offset and the length must be multiples of
    // four, and no pass may be open; the length is clamped to the buffer's end,
    // and a range starting at or past that end fills nothing.
    void fill(const BufferRange& range, std::uint8_t value = 0);

    void fill(const Buffer& buffer, std::uint8_t value = 0)
    {
        fill(BufferRange::of(buffer), value);
    }

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

    // What the GPU spent on this buffer's labelled passes, and on the buffer end
    // to end. Empty until the work has finished - after commit(), or after the
    // Async from commitAsync() resolved - and on a buffer that labelled nothing.
    const FrameTimings& timings();

    // The off-screen sibling of Device::supportsPassTimings(): false says the
    // per-pass breakdown will be empty, not that timings() reports nothing.
    // Answerable once a labelled pass has begun.
    bool supportsPassTimings() const;

    bool isValid() const;

private:
    struct Native;
    Pimpl<Native> impl;
};
} // namespace eacp::GPU

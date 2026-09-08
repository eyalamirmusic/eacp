#pragma once

#include "FrameTimings.h"
#include "GpuTimestamps.h"

#include <optional>
#include <string_view>

namespace eacp::GPU
{
class Device;

// Times the labelled passes of one off-screen command buffer, and the buffer end
// to end. FrameTimer's sibling for work with no frame around it: one slot rather
// than a rotation, since a command buffer reads its own numbers rather than an
// earlier submission's. Owned by CommandBuffer and driven entirely by it.
class CommandTimer
{
public:
    // The pass's ordinal - its two samples are at 2 * ordinal and 2 * ordinal + 1
    // - or -1 when the pass is not being timed, which is what an unlabelled pass,
    // an unsupported device and a buffer already holding maxTimedPasses labelled
    // passes all get.
    int beginPass(std::string_view label, Device& device, void* nativeCommandBuffer);

    // Where a timed pass writes its samples. Null when nothing is being timed.
    void* nativeSamples() const;

    // endRecording before the submission, since it records onto the command
    // buffer, and noteSubmitted after it.
    void endRecording(void* nativeCommandBuffer);
    void noteSubmitted(std::uint64_t fenceValue);

    // Empty until the GPU has finished the buffer, and read off the samples the
    // first time it has.
    const FrameTimings& timings(const Device& device);

    // See GpuTimestamps::isSupported. False until a labelled pass has begun.
    bool isSupported() const;

private:
    // The one slot a command buffer needs, against the four a frame rotates
    // through.
    static constexpr int slot = 0;

    std::optional<GpuTimestamps> timestamps;

    Vector<std::string> labels;
    FrameTimings latest;

    int passCount = 0;
    bool pending = false;
};
} // namespace eacp::GPU

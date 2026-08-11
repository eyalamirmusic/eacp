#pragma once

#include "FrameTimings.h"
#include "GpuTimestamps.h"

#include <string_view>

namespace eacp::GPU
{
// Times each labelled pass and hands back the most recent frame whose numbers
// arrived. Owned by Device, driven by Frame. A frame writes into slot
// frameIndex % slotCount, read only once the GPU has finished it.
class FrameTimer
{
public:
    // Called by Device::beginFrame, which every Frame constructor runs.
    void beginFrame(std::uint64_t frameIndex);

    // Returns the pass's ordinal in this frame, its two samples being at
    // 2 * ordinal and 2 * ordinal + 1, or -1 when the pass is not timed.
    int beginPass(std::string_view label);

    // Called by Frame's destructor: endFrame before it commits, noteSubmitted
    // after, a D3D12 fence value not existing until then.
    void endFrame(void* nativeCommandBuffer);
    void noteSubmitted(std::uint64_t fenceValue);

    // Null when this frame is not being timed.
    void* nativeSamples() const;

    // See GpuTimestamps::beginRecording.
    void beginRecording(void* nativeCommandBuffer);

    // Empty until the GPU has finished and reported a frame.
    const FrameTimings& lastTimings() const { return latest; }

    // Only meaningful once a frame has begun, which builds the resources.
    bool isSupported() const { return timestamps.isSupported(); }

private:
    void drainCompleted();

    struct Slot
    {
        // Kept across frames so repeated labels assign into existing capacity.
        Vector<std::string> labels;

        std::uint64_t frameIndex = 0;
        int passCount = 0;
        bool pending = false;
    };

    GpuTimestamps timestamps;

    Array<Slot, GpuTimestamps::slotCount> slots;

    FrameTimings latest;

    // The slot the frame being encoded writes into; -1 when it is not timed.
    int current = -1;
    int passCount = 0;
};
} // namespace eacp::GPU

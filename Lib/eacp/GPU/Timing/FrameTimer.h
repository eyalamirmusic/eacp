#pragma once

#include "FrameTimings.h"
#include "GpuTimestamps.h"

#include <string_view>

namespace eacp::GPU
{
// Times each labelled pass of a frame on the GPU, and hands back the most
// recent frame whose numbers have arrived.
//
// Owned by Device and driven entirely by Frame, so an app has nothing to call
// and therefore nothing to forget - the same instinct as StreamingBuffers
// taking its pool from the device's frame counter rather than from a call at
// the frame boundary.
//
// The slots rotate. A frame writes its timestamps into the slot for
// frameIndex % slotCount, and a slot is read only once the backend says the GPU
// has finished the frame that wrote it. That is what makes the results late,
// and it is also what makes them free: nothing anywhere waits for the GPU.
class FrameTimer
{
public:
    // Called by Device::beginFrame, which every Frame constructor runs.
    void beginFrame(std::uint64_t frameIndex);

    // Called by Frame::beginPass and Frame::beginCompute for a pass carrying a
    // label. Returns the pass's ordinal in this frame - its two samples are at
    // 2 * ordinal and 2 * ordinal + 1 - or -1 when the pass is not being
    // timed, which is what an unlabelled pass, an unsupported device and a
    // frame already holding maxTimedPasses labelled passes all get.
    int beginPass(std::string_view label);

    // Called by Frame's destructor: endFrame before it commits, noteSubmitted
    // after, since a D3D12 fence value does not exist until then.
    void endFrame(void* nativeCommandBuffer);
    void noteSubmitted(std::uint64_t fenceValue);

    // Where this frame's passes write their samples, for a backend that has to
    // name it when the pass begins. Null when this frame is not being timed.
    void* nativeSamples() const;

    // See GpuTimestamps::beginRecording.
    void beginRecording(void* nativeCommandBuffer);

    // The most recent frame the GPU has finished and reported. Empty - no
    // passes, and a zero frameIndex - until one has.
    const FrameTimings& lastTimings() const { return latest; }

    // See GpuTimestamps::isSupported. Only meaningful once a frame has begun,
    // which is what builds the timestamp resources.
    bool isSupported() const { return timestamps.isSupported(); }

private:
    void drainCompleted();

    struct Slot
    {
        // Kept across frames rather than cleared, so that a steady stream of
        // frames labelling their passes the same way assigns into strings that
        // already have the capacity and allocates nothing.
        Vector<std::string> labels;

        std::uint64_t frameIndex = 0;
        int passCount = 0;
        bool pending = false;
    };

    GpuTimestamps timestamps;

    Array<Slot, GpuTimestamps::slotCount> slots;

    FrameTimings latest;

    // The slot the frame being encoded writes into, and how many labelled
    // passes it has had so far. -1 when the frame is not being timed.
    int current = -1;
    int passCount = 0;
};
} // namespace eacp::GPU

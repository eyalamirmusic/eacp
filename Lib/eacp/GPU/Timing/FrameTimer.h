#pragma once

#include "FrameTimings.h"
#include "GpuTimestamps.h"

#include <string_view>

namespace eacp::GPU
{
class Device;

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
    // Called by Device::beginFrame, which every Frame constructor runs, and
    // which passes the Device these frames belong to — the backend needs it to
    // name the queue whose fence says when a slot is readable.
    void beginFrame(std::uint64_t frameIndex, Device& device);

    // Called by Frame::beginPass and Frame::beginCompute for a pass carrying a
    // label. Returns the pass's ordinal in this frame - which samples are its
    // is the backend's layout, two a pass on D3D12 and Vulkan and four on
    // Metal - or -1 when the pass is not being timed, which is what an unlabelled pass, an unsupported device and a
    // frame already holding maxTimedPasses labelled passes all get.
    int beginPass(std::string_view label);

    // Times every pass begun until the next call under `scope`, an unlabelled
    // one as `scope` itself and a labelled one as "scope/label"; empty turns it
    // off. Returns the scope it replaced. What Frame::Redirect's timing label
    // sets, and the view has to outlive it.
    std::string_view setScope(std::string_view scope);

    // Called by Frame's destructor: endFrame before it commits, noteSubmitted
    // after, since a D3D12 fence value does not exist until then.
    void endFrame(void* nativeCommandBuffer);
    void noteSubmitted(std::uint64_t fenceValue);

    // Where this frame's passes write their samples, for a backend that has to
    // name it when the pass begins. Null when this frame is not being timed.
    void* nativeSamples() const;

    // Stops timing the frame in progress and hands back the slot it was timing
    // into, for resume to restore: what a frame nested in another
    // (Device::enterFrame) does, so its passes are timed by no one rather than
    // by the frame around it.
    int suspend();
    void resume(int slot);

    // See GpuTimestamps::beginRecording.
    void beginRecording(void* nativeCommandBuffer);

    // The most recent frame the GPU has finished and reported. Empty - no
    // passes, and a zero frameIndex - until one has.
    const FrameTimings& lastTimings() const;

    // See GpuTimestamps::isSupported. Only meaningful once a frame has begun,
    // which is what builds the timestamp resources.
    bool isSupported() const;

private:
    void drainCompleted(const Device& device);

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

    std::string_view scope;

    // The slot the frame being encoded writes into, and how many labelled
    // passes it has had so far. -1 when the frame is not being timed.
    int current = -1;
    int passCount = 0;
};
} // namespace eacp::GPU

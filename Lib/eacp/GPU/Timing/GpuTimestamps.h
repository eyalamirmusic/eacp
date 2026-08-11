#pragma once

#include <eacp/Core/Utils/Pimpl.h>

#include <cstdint>

namespace eacp::GPU
{
// The backend half of frame timing: a fixed set of slots, each holding one
// in-flight frame's raw GPU timestamps. Knows nothing of pass names or which
// frame is current - that is FrameTimer's portable half.
class GpuTimestamps
{
public:
    GpuTimestamps();
    ~GpuTimestamps();

    GpuTimestamps(const GpuTimestamps&) = delete;
    GpuTimestamps& operator=(const GpuTimestamps&) = delete;

    // False until the first beginSlot, which creates the resources. May also
    // return to false once, when resolveSlot catches an adapter that advertised
    // timestamps and writes none.
    bool isSupported() const;

    // Forgets what the slot held and prepares it for a frame's samples.
    void beginSlot(int slot);

    // The frame's own start timestamp, a D3D12 need; Metal reads GPUStartTime
    // off the command buffer afterwards and does nothing here.
    void beginRecording(int slot, void* nativeCommandBuffer);

    // An MTLCounterSampleBuffer on Metal, an ID3D12QueryHeap on D3D12. Null
    // when timing is off or unsupported.
    void* nativeSamples(int slot) const;

    // Call before committing. Returns whether the slot will have something to
    // report; on false the caller must not leave it pending, a slot that never
    // completes never being recycled. Metal answers true even unsupported.
    bool endSlot(int slot, int passCount, void* nativeCommandBuffer);

    // Does nothing on Metal, where the retained command buffer reports its own
    // status; D3D12 only learns the fence value after execution.
    void noteSubmitted(int slot, std::uint64_t fenceValue);

    // Reading a slot before this is true reads timestamps the GPU has not
    // written yet.
    bool isSlotComplete(int slot) const;

    // Fills milliseconds[i] per pass and returns the frame's GPU time, or zero
    // where the backend cannot say. Non-const because an entirely unwritten
    // slot retires isSupported() - some virtualised GPUs never write one.
    double resolveSlot(int slot, int passCount, double* milliseconds);

    // One more than the deepest pipeline either backend runs, so the frame being
    // encoded never lands on a slot whose results are still owed.
    static constexpr int slotCount = 4;

    // Passes past this draw normally and are simply not timed.
    static constexpr int maxTimedPasses = 16;

private:
    struct Native;
    Pimpl<Native> impl;
};
} // namespace eacp::GPU

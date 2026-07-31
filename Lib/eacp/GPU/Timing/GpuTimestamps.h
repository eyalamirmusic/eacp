#pragma once

#include <eacp/Core/Utils/Pimpl.h>

#include <cstdint>

namespace eacp::GPU
{
class Device;

// The backend half of frame timing: a fixed set of slots, each holding one
// in-flight frame's raw GPU timestamps.
//
// This is where the two backends stop resembling each other. Metal writes
// timestamps into an MTLCounterSampleBuffer named by a pass descriptor, and
// only ever at a pass boundary - Apple silicon supports counter sampling at
// MTLCounterSamplingPointAtStageBoundary and at none of the others, so there is
// no such thing as a timestamp in the middle of a pass. D3D12 writes them into
// a query heap at any point on the command list, and needs that heap resolved
// into a readback buffer by the list itself before anything can be read.
//
// So the seam is drawn here, and FrameTimer above it is portable. Nothing in
// this class knows what a pass is called or which frame is current.
class GpuTimestamps
{
public:
    GpuTimestamps();
    ~GpuTimestamps();

    GpuTimestamps(const GpuTimestamps&) = delete;
    GpuTimestamps& operator=(const GpuTimestamps&) = delete;

    // Whether per-pass timing works on this device at all: whether it has a
    // timestamp counter set and will sample it where the passes are. False
    // until the first beginSlot, which is what creates the resources - a
    // Device cannot build them in its own constructor, since it is not yet
    // itself when that runs.
    //
    // It can also go back to false, once, having been true: an adapter is only
    // taken at its word until a resolved slot proves otherwise. See resolveSlot.
    bool isSupported() const;

    // Forgets what the slot held and prepares it for a frame's samples. The
    // Device is the one whose frames these are: it owns the query heaps on
    // D3D12, and the fence they are read against. Metal needs neither and
    // ignores it.
    void beginSlot(int slot, Device& device);

    // Records the frame's own start timestamp, which is a D3D12 need: its total
    // is two queries on the command list like everything else. Metal reads
    // GPUStartTime off the command buffer afterwards and does nothing here.
    void beginRecording(int slot, void* nativeCommandBuffer);

    // What a pass attaches its samples to: an MTLCounterSampleBuffer on Metal,
    // an ID3D12QueryHeap on D3D12. Null when timing is off or unsupported.
    void* nativeSamples(int slot) const;

    // Records whatever has to be on the command buffer before it is committed -
    // the closing timestamp and a ResolveQueryData on D3D12, nothing on Metal -
    // and takes hold of what will say when the GPU has finished with the slot.
    //
    // Returns whether the slot is now waiting on the GPU and will have
    // something to report. False means it never will, and the caller must not
    // leave it pending: a slot that can never complete is never recycled, and
    // four of those stop the timer for the rest of the process.
    //
    // The two backends differ here, which is why this is a question rather than
    // an assumption. Metal still has the frame's own total without any counter
    // support at all, so an unsupported device answers true and reports that
    // much; D3D12 measures the frame with the same queries as everything else,
    // so without them it answers false and reports nothing.
    bool endSlot(int slot, int passCount, void* nativeCommandBuffer);

    // The fence value the submission was given, which D3D12 only learns after
    // the list has been executed. Does nothing on Metal, where the command
    // buffer reports its own status and was retained by endSlot.
    void noteSubmitted(int slot, std::uint64_t fenceValue);

    // Whether the GPU has finished the frame that wrote this slot, so its
    // samples can be read. Reading one before this is true reads a timestamp
    // the GPU has not written yet. The Device is the one that submitted it,
    // whose fence answers the question on D3D12.
    bool isSlotComplete(int slot, const Device& device) const;

    // Reads a completed slot back. milliseconds[i] is filled for each of the
    // passCount passes; the return value is the whole frame's GPU time, or zero
    // where the backend cannot say.
    //
    // Not const, because reading a slot is also the only chance to find out that
    // an adapter which advertised timestamps does not actually write any - some
    // virtualised GPUs report a frequency, accept the query heap and then leave
    // the readback untouched. A slot that comes back entirely unwritten retires
    // isSupported(), so a caller is told it cannot profile instead of being
    // handed a column of zeroes.
    double resolveSlot(int slot, int passCount, double* milliseconds);

    // How many slots there are, and therefore how many frames may be timed at
    // once. One more than the deepest pipeline either backend runs, so the
    // frame being encoded never lands on a slot whose results are still owed.
    static constexpr int slotCount = 4;

    // The per-frame ceiling on labelled passes. A pass past it draws exactly as
    // it would have and is simply not timed - see FrameTimer::beginPass, which
    // is where that is decided and where it would be noticed.
    static constexpr int maxTimedPasses = 16;

private:
    struct Native;
    Pimpl<Native> impl;
};
} // namespace eacp::GPU

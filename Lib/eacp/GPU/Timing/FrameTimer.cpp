#include "FrameTimer.h"

namespace eacp::GPU
{
void FrameTimer::beginFrame(std::uint64_t frameIndex)
{
    // Before the new frame takes a slot, so that the one it is about to take is
    // read first if the GPU has since finished with it.
    drainCompleted();

    const auto next = (int) (frameIndex % (std::uint64_t) GpuTimestamps::slotCount);

    // A slot whose frame has not come back yet is still owed a read, and
    // recording over it would lose that frame's numbers and read a mixture of
    // two frames' timestamps for this one. Leaving the new frame untimed costs
    // a gap in the graph, which is the version that is not wrong.
    if (slots[next].pending)
    {
        current = -1;
        return;
    }

    current = next;
    passCount = 0;

    slots[current].frameIndex = frameIndex;

    timestamps.beginSlot(current);
}

int FrameTimer::beginPass(std::string_view label)
{
    if (current < 0 || label.empty() || !timestamps.isSupported())
        return -1;

    if (passCount >= GpuTimestamps::maxTimedPasses)
        return -1;

    auto& labels = slots[current].labels;

    if (labels.size() <= passCount)
        labels.resize(passCount + 1);

    labels[passCount].assign(label);

    return passCount++;
}

void FrameTimer::beginRecording(void* nativeCommandBuffer)
{
    if (current >= 0)
        timestamps.beginRecording(current, nativeCommandBuffer);
}

void* FrameTimer::nativeSamples() const
{
    return current < 0 ? nullptr : timestamps.nativeSamples(current);
}

void FrameTimer::endFrame(void* nativeCommandBuffer)
{
    if (current < 0)
        return;

    // Pending only if the backend will actually have an answer. A slot left
    // waiting on one that never comes is never recycled, and four of those is
    // the timer switched off for the rest of the process - which is exactly
    // what a device with no counter support used to do.
    slots[current].passCount = passCount;
    slots[current].pending =
        timestamps.endSlot(current, passCount, nativeCommandBuffer);
}

void FrameTimer::noteSubmitted(std::uint64_t fenceValue)
{
    if (current >= 0)
        timestamps.noteSubmitted(current, fenceValue);
}

void FrameTimer::drainCompleted()
{
    for (auto slot = 0; slot < GpuTimestamps::slotCount; ++slot)
    {
        auto& entry = slots[slot];

        if (!entry.pending || !timestamps.isSlotComplete(slot))
            continue;

        double milliseconds[GpuTimestamps::maxTimedPasses] = {};
        const auto frameTime =
            timestamps.resolveSlot(slot, entry.passCount, milliseconds);

        entry.pending = false;

        // Two slots can complete in one drain - a frame that was never
        // presented, a stall - and the newer of them is the answer. Without
        // this the reported frame would go backwards, which reads as the
        // profiler being wrong rather than the frames arriving together.
        if (entry.frameIndex < latest.frameIndex)
            continue;

        latest.frameIndex = entry.frameIndex;
        latest.milliseconds = frameTime;
        latest.passes.resize(entry.passCount);

        for (auto pass = 0; pass < entry.passCount; ++pass)
        {
            latest.passes[pass].label = entry.labels[pass];
            latest.passes[pass].milliseconds = milliseconds[pass];
        }
    }
}
} // namespace eacp::GPU

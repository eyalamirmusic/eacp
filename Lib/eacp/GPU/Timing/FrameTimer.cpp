#include "FrameTimer.h"

namespace eacp::GPU
{
void FrameTimer::beginFrame(std::uint64_t frameIndex)
{
    // Before taking a slot, so this frame's own is read first if it is ready.
    drainCompleted();

    const auto next = (int) (frameIndex % (std::uint64_t) GpuTimestamps::slotCount);

    // Recording over a pending slot would mix two frames' timestamps; leaving
    // this frame untimed only costs a gap.
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

    // Pending only if an answer is coming: a slot waiting on one that never
    // arrives is never recycled, and slotCount of those stop the timer.
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

        // Two slots can complete in one drain; keep the newer so the reported
        // frame never goes backwards.
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

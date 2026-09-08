#include "CommandTimer.h"

namespace eacp::GPU
{
int CommandTimer::beginPass(std::string_view label,
                            Device& device,
                            void* nativeCommandBuffer)
{
    if (label.empty() || passCount >= GpuTimestamps::maxTimedPasses)
        return -1;

    // The first labelled pass builds the resources, so a command buffer nobody
    // asked to time creates none.
    if (!timestamps.has_value())
    {
        timestamps.emplace();
        timestamps->beginSlot(slot, device);
        timestamps->beginRecording(slot, nativeCommandBuffer);
    }

    if (!timestamps->isSupported())
        return -1;

    if (labels.size() <= passCount)
        labels.resize(passCount + 1);

    labels[passCount].assign(label);

    return passCount++;
}

void* CommandTimer::nativeSamples() const
{
    return timestamps.has_value() ? timestamps->nativeSamples(slot) : nullptr;
}

void CommandTimer::endRecording(void* nativeCommandBuffer)
{
    if (timestamps.has_value())
        pending = timestamps->endSlot(slot, passCount, nativeCommandBuffer);
}

void CommandTimer::noteSubmitted(std::uint64_t fenceValue)
{
    if (timestamps.has_value())
        timestamps->noteSubmitted(slot, fenceValue);
}

const FrameTimings& CommandTimer::timings(const Device& device)
{
    if (!pending || !timestamps->isSlotComplete(slot, device))
        return latest;

    double milliseconds[GpuTimestamps::maxTimedPasses] = {};

    latest.milliseconds = timestamps->resolveSlot(slot, passCount, milliseconds);
    latest.passes.resize(passCount);

    for (auto pass = 0; pass < passCount; ++pass)
    {
        latest.passes[pass].label = labels[pass];
        latest.passes[pass].milliseconds = milliseconds[pass];
    }

    pending = false;

    return latest;
}

bool CommandTimer::isSupported() const
{
    return timestamps.has_value() && timestamps->isSupported();
}
} // namespace eacp::GPU

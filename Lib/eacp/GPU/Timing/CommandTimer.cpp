#include "CommandTimer.h"

#include <algorithm>

namespace eacp::GPU
{
int CommandTimer::beginPass(std::string_view label,
                            Device& device,
                            void* nativeCommandBuffer)
{
    if (label.empty())
        return -1;

    // A command buffer nobody asked to time creates nothing; one that runs past
    // a set of samples gets another.
    if (passCount == (int) chunks.size() * GpuTimestamps::maxTimedPasses)
    {
        auto chunk = std::make_unique<GpuTimestamps>();
        chunk->beginSlot(slot, device);
        chunk->beginRecording(slot, nativeCommandBuffer);
        chunks.push_back(std::move(chunk));
    }

    if (!chunks.front()->isSupported())
        return -1;

    if (labels.size() <= passCount)
        labels.resize(passCount + 1);

    labels[passCount].assign(label);

    return passCount++ % GpuTimestamps::maxTimedPasses;
}

void* CommandTimer::nativeSamples() const
{
    return chunks.empty() ? nullptr : chunks.back()->nativeSamples(slot);
}

void CommandTimer::endRecording(void* nativeCommandBuffer)
{
    for (auto chunk = 0; chunk < (int) chunks.size(); ++chunk)
        pending = chunks[(std::size_t) chunk]->endSlot(
                      slot, passesIn(chunk), nativeCommandBuffer)
                  || pending;
}

void CommandTimer::noteSubmitted(std::uint64_t fenceValue)
{
    for (auto& chunk: chunks)
        chunk->noteSubmitted(slot, fenceValue);
}

int CommandTimer::passesIn(int chunk) const
{
    auto before = chunk * GpuTimestamps::maxTimedPasses;
    return std::min(GpuTimestamps::maxTimedPasses, passCount - before);
}

const FrameTimings& CommandTimer::timings(const Device& device)
{
    if (!pending)
        return latest;

    for (const auto& chunk: chunks)
        if (!chunk->isSlotComplete(slot, device))
            return latest;

    auto milliseconds =
        std::vector<double>((std::size_t) GpuTimestamps::maxTimedPasses);
    latest.passes.resize(passCount);

    for (auto chunk = 0; chunk < (int) chunks.size(); ++chunk)
    {
        auto count = passesIn(chunk);
        auto total = chunks[(std::size_t) chunk]->resolveSlot(
            slot, count, milliseconds.data());

        if (chunk == 0)
            latest.milliseconds = total;

        for (auto pass = 0; pass < count; ++pass)
        {
            auto index = chunk * GpuTimestamps::maxTimedPasses + pass;
            latest.passes[index].label = labels[index];
            latest.passes[index].milliseconds = milliseconds[(std::size_t) pass];
        }
    }

    pending = false;

    return latest;
}

bool CommandTimer::isSupported() const
{
    return !chunks.empty() && chunks.front()->isSupported();
}
} // namespace eacp::GPU

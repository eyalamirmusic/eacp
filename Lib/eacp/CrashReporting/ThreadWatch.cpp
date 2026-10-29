#include "ThreadWatch.h"
#include "CrashReportingPlatform.h"

namespace eacp::CrashReporting
{
namespace
{
std::optional<std::size_t> claimSlot(std::string_view name,
                                     std::chrono::milliseconds threshold)
{
    auto hang = Detail::findWatchedHangInfo();

    if (!hang)
        return std::nullopt;

    auto slot = Detail::claimWatchedThread(hang->get(),
                                           name,
                                           Detail::countMilliseconds(threshold),
                                           Detail::getCurrentThreadId());

    if (slot)
        Detail::recordThreadForSampling(hang->get().threads[*slot]);

    return slot;
}

template <typename SlotFunction>
void applyToSlot(const std::optional<std::size_t>& slot, SlotFunction apply)
{
    if (!slot)
        return;

    if (auto hang = Detail::findWatchedHangInfo())
        apply(hang->get().threads[*slot]);
}
} // namespace

ThreadWatch::ThreadWatch(std::string_view name, std::chrono::milliseconds threshold)
    : slot(claimSlot(name, threshold))
{
}

ThreadWatch::~ThreadWatch()
{
    applyToSlot(slot, Detail::releaseWatchedThread);
}

void ThreadWatch::beat()
{
    applyToSlot(slot, Detail::beatWatchedThread);
}

void ThreadWatch::pause()
{
    applyToSlot(slot, Detail::pauseWatchedThread);
}

void ThreadWatch::resume()
{
    applyToSlot(slot, Detail::resumeWatchedThread);
}

bool ThreadWatch::isWatched() const
{
    return slot.has_value();
}
} // namespace eacp::CrashReporting

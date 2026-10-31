#include "CrashReporting.h"
#include "CrashReportingPlatform.h"

#include <eacp/Core/Utils/Strings.h>

#include <limits>

namespace eacp::CrashReporting
{
namespace
{
constexpr auto slotNotClaimed = std::numeric_limits<std::size_t>::max();
constexpr auto noSlotWasFree = Detail::maxWatchedThreads;

// One claim per thread: a thread that found the table full stays unwatched
// rather than searching it again on every heartbeat.
thread_local auto currentThreadSlot = slotNotClaimed;

std::string getNameForReport()
{
    auto name = Detail::getCurrentThreadName();

    if (name.empty())
        return Strings::concat("thread ", Detail::getCurrentThreadId());

    return name;
}

std::size_t claimSlotForCurrentThread(Detail::SharedHangInfo& hang)
{
    auto slot = Detail::claimWatchedThread(
        hang, getNameForReport(), Detail::getCurrentThreadId());

    if (!slot)
        return noSlotWasFree;

    Detail::recordThreadForSampling(hang.threads[*slot]);
    return *slot;
}

template <typename SlotFunction>
void applyToCurrentThreadSlot(SlotFunction apply)
{
    if (currentThreadSlot >= Detail::maxWatchedThreads)
        return;

    if (auto hang = Detail::findWatchedHangInfo())
        apply(hang->get().threads[currentThreadSlot]);
}
} // namespace

void sendHeartbeat()
{
    if (currentThreadSlot == slotNotClaimed)
    {
        auto hang = Detail::findWatchedHangInfo();

        if (!hang || !Detail::isWatchingWorkerThreads())
            return;

        currentThreadSlot = claimSlotForCurrentThread(hang->get());
    }

    applyToCurrentThreadSlot(Detail::sendWatchedThreadHeartbeat);
}

void pauseHangDetection()
{
    applyToCurrentThreadSlot(Detail::pauseWatchedThread);
}

void resumeHangDetection()
{
    applyToCurrentThreadSlot(Detail::resumeWatchedThread);
}
} // namespace eacp::CrashReporting

namespace eacp::CrashReporting::Detail
{
void useMainSlotOnCurrentThread()
{
    currentThreadSlot = mainThreadSlot;
}

void releaseWatchOfThread(std::uint64_t threadId)
{
    if (auto hang = findWatchedHangInfo())
        releaseWatchedThreadWithId(hang->get(), threadId);
}
} // namespace eacp::CrashReporting::Detail

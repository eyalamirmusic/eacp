#include "DisplayLink.h"

#include "../Window/LinuxWindowSystem-Linux.h"

#include <eacp/Core/Threads/ThreadUtils.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <ctime>
#include <memory>
#include <thread>

namespace eacp::Threads
{
namespace
{
// A clock, not a window-system signal: what the compositor or the server says
// about a frame is per-surface and this is process-wide. GPU views are paced
// by ViewSurface::onFrameDone instead.
constexpr long linuxNanosecondsPerSecond = 1'000'000'000;
constexpr long linuxDisplayLinkFallbackPeriodNs = 16'666'667;

// Outside this, the reported mode is not worth pacing a timer against.
constexpr int linuxDisplayLinkMinHz = 24;
constexpr int linuxDisplayLinkMaxHz = 480;

long linuxDisplayLinkPeriodNs()
{
    auto output = Graphics::linuxPrimaryOutput();

    if (!output || output->refreshMilliHz <= 0)
        return linuxDisplayLinkFallbackPeriodNs;

    auto hz = output->refreshMilliHz / 1000;

    if (hz < linuxDisplayLinkMinHz || hz > linuxDisplayLinkMaxHz)
        return linuxDisplayLinkFallbackPeriodNs;

    return (long) ((int64_t) linuxNanosecondsPerSecond * 1000
                   / output->refreshMilliHz);
}

// Shared with queued ticks, so one outliving the link touches no dead callback.
struct LinuxDisplayLinkTick
{
    explicit LinuxDisplayLinkTick(const Callback& cbToUse)
        : cb(cbToUse)
    {
    }

    Callback cb;
    std::atomic<bool> alive {true};
    std::atomic<bool> pending {false};
};

void advanceLinuxTickDeadline(timespec& deadline, long nanoseconds)
{
    deadline.tv_nsec += nanoseconds;

    while (deadline.tv_nsec >= linuxNanosecondsPerSecond)
    {
        deadline.tv_nsec -= linuxNanosecondsPerSecond;
        ++deadline.tv_sec;
    }
}
} // namespace

struct DisplayLink::Native
{
    explicit Native(const Callback& cb)
        : state(std::make_shared<LinuxDisplayLinkTick>(cb))
        , periodNs(linuxDisplayLinkPeriodNs())
    {
        assertMainThread();

        // periodNs is read on the main thread before the pacing thread starts:
        // the window system's connection is not thread-safe to interrogate.
        thread = std::thread([this] { tickLoop(); });
    }

    ~Native()
    {
        assertMainThread();

        state->alive = false;
        stopped = true;
        thread.join();
    }

    void tickLoop()
    {
        auto deadline = timespec {};
        clock_gettime(CLOCK_MONOTONIC, &deadline);

        while (!stopped)
        {
            advanceLinuxTickDeadline(deadline, periodNs);

            while (
                clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &deadline, nullptr)
                == EINTR)
            {
            }

            if (stopped)
                break;

            postTick();
        }
    }

    void postTick() const
    {
        if (state->pending.exchange(true))
            return;

        callAsync(
            [tick = state]
            {
                tick->pending = false;

                if (tick->alive)
                    tick->cb();
            });
    }

    std::shared_ptr<LinuxDisplayLinkTick> state;
    long periodNs = linuxDisplayLinkFallbackPeriodNs;
    std::atomic<bool> stopped {false};
    std::thread thread;
};

DisplayLink::DisplayLink(const FrameCallback& cb)
    : rateLimit(std::make_shared<RateLimit>())
    , callback(rateLimited(rateLimit, timedTick(cb)))
    , impl(callback)
{
}

} // namespace eacp::Threads

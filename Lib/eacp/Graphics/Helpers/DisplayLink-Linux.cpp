#include "DisplayLink.h"

#include <eacp/Core/Threads/ThreadUtils.h>

#include <atomic>
#include <cerrno>
#include <ctime>
#include <memory>
#include <thread>

namespace eacp::Threads
{
namespace
{
// 60 Hz, because there is no output to ask. This is the same fallback
// DisplayLink-Windows.cpp drops to when the session has no compositor clock,
// and it has the same property: the cadence is honest about being a guess, so a
// caller that scales by FrameTime::delta is unaffected by it being wrong.
//
// The real rate comes from the surface rather than from a clock — Wayland's
// wl_surface.frame fires when the compositor is ready for the next buffer of
// THAT surface, which is per-window and can differ between outputs. Wiring it
// up therefore needs a DisplayLink that knows which view it belongs to, so it
// waits for stage 4 rather than being approximated better here.
constexpr long linuxDisplayLinkPeriodNs = 16'666'667;
constexpr long linuxNanosecondsPerSecond = 1'000'000'000;

// Shared between the pacing thread, ticks already queued on the main thread and
// the link itself, so a tick still in the queue when the link is destroyed
// fizzles rather than touching a dead callback.
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

// Paces a thread against CLOCK_MONOTONIC and posts each tick to the main
// thread. Absolute deadlines rather than a sleep per frame, so a slow tick
// costs one frame instead of shifting every frame after it.
//
// Ticks coalesce: while one is still queued behind a busy main thread, further
// deadlines are skipped rather than piling up — the same rule as the Apple and
// Windows links, and the reason a stalled callback cannot fill the queue.
struct DisplayLink::Native
{
    explicit Native(const Callback& cb)
        : state(std::make_shared<LinuxDisplayLinkTick>(cb))
    {
        assertMainThread();

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
            advanceLinuxTickDeadline(deadline, linuxDisplayLinkPeriodNs);

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

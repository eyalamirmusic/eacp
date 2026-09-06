#include "DisplayLink.h"

#include "../Window/WaylandDisplay-Linux.h"

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
// A clock, not a compositor signal, and deliberately still so.
//
// This stays a paced thread even now that there is a compositor to talk to,
// because the thing a compositor offers instead is per-surface:
// wl_surface.frame fires when it is ready for the next buffer of THAT surface,
// which differs between windows and between outputs, and a process-wide
// DisplayLink has no surface to name. Continuous GPU rendering is paced by the
// per-view callback the ViewSurface contract carries (onFrameDone) rather than
// by this; what is left for a clock is everything else that wants a steady
// tick, and for that a guess honest about being a guess is enough - a caller
// scaling by FrameTime::delta is unaffected by the cadence being wrong.
//
// The one thing worth asking the compositor is the rate: an output that says it
// runs at 144 Hz is worth pacing against, and the mode is already on hand.
constexpr long linuxNanosecondsPerSecond = 1'000'000'000;
constexpr long linuxDisplayLinkFallbackPeriodNs = 16'666'667;

// Anything outside this is a compositor reporting nonsense, or a mode nothing
// good comes of pacing a general-purpose timer against.
constexpr int linuxDisplayLinkMinHz = 24;
constexpr int linuxDisplayLinkMaxHz = 480;

long linuxDisplayLinkPeriodNs()
{
    auto* connection = Graphics::waylandDisplay();

    if (connection == nullptr)
        return linuxDisplayLinkFallbackPeriodNs;

    const auto* output = connection->getPrimaryOutput();

    if (output == nullptr || output->refreshMilliHz <= 0)
        return linuxDisplayLinkFallbackPeriodNs;

    auto hz = output->refreshMilliHz / 1000;

    if (hz < linuxDisplayLinkMinHz || hz > linuxDisplayLinkMaxHz)
        return linuxDisplayLinkFallbackPeriodNs;

    return (long) ((int64_t) linuxNanosecondsPerSecond * 1000
                   / output->refreshMilliHz);
}

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
        , periodNs(linuxDisplayLinkPeriodNs())
    {
        assertMainThread();

        // Read once, on the main thread, before the pacing thread starts: the
        // Wayland connection is not thread-safe to interrogate, and a display
        // whose mode changes under a running link is a rarity this does not
        // chase.
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

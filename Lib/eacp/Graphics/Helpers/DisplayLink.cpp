#include "DisplayLink.h"
#include <algorithm>
#include <chrono>

namespace eacp::Threads
{
DisplayLink::DisplayLink(const Callback& cb)
    : DisplayLink(FrameCallback([cb](FrameTime) { cb(); }))
{
}

void DisplayLink::setMaxFps(int fps)
{
    rateLimit->fps = fps;
    rateLimit->minInterval = fps > 0 ? 1.0 / fps : 0.0;
}

int DisplayLink::maxFps() const
{
    return rateLimit->fps;
}

// The divider that implements setMaxFps. Skips ticks until the cap's
// interval has accumulated, firing on whichever tick lands closest to due:
// waiting for the next one when this one is within half a refresh of the
// target would overshoot by more than firing now undershoots. Without that
// half-tick grace, 60 on a 120Hz panel misses the interval by float dust
// and runs at 40.
Callback DisplayLink::rateLimited(const std::shared_ptr<RateLimit>& limit,
                                  const Callback& tick)
{
    using Clock = std::chrono::steady_clock;

    struct PacingState
    {
        Clock::time_point lastTick;
        double accumulated = 0.0;
        bool started = false;
    };

    auto state = std::make_shared<PacingState>();

    return [limit, state, tick]
    {
        const auto interval = limit->minInterval.load();

        if (interval <= 0.0)
        {
            state->started = false;
            tick();
            return;
        }

        const auto now = Clock::now();

        if (!state->started)
        {
            state->started = true;
            state->lastTick = now;
            state->accumulated = interval;
        }

        const auto period =
            std::chrono::duration<double>(now - state->lastTick).count();
        state->lastTick = now;
        state->accumulated += period;

        if (state->accumulated + period * 0.5 < interval)
            return;

        // Carry the remainder so an uneven cap holds as an average; cap the
        // bank at half an interval so a stall buys one prompt frame rather
        // than a burst of catch-up frames.
        state->accumulated = std::min(state->accumulated - interval, interval * 0.5);
        tick();
    };
}

// Stamps each tick with the time since the link started and since the
// previous tick. The state lives in the returned callback itself, so the
// platform Natives stay timing-agnostic.
Callback DisplayLink::timedTick(const FrameCallback& cb)
{
    using Clock = std::chrono::steady_clock;

    struct TimingState
    {
        Clock::time_point start;
        Clock::time_point last;
        bool started = false;
    };

    auto state = std::make_shared<TimingState>();

    return [cb, state]
    {
        auto now = Clock::now();

        if (!state->started)
        {
            state->start = now;
            state->last = now;
            state->started = true;
        }

        // Across a stall (paused links, a blocked main thread) the gap can be
        // arbitrarily long; clamping keeps the first frame after it a normal
        // animation step instead of a jump.
        constexpr auto maxDelta = 0.1;

        auto frame = FrameTime {};
        frame.time = std::chrono::duration<double>(now - state->start).count();
        frame.delta = std::min(
            std::chrono::duration<double>(now - state->last).count(), maxDelta);
        state->last = now;

        cb(frame);
    };
}
} // namespace eacp::Threads

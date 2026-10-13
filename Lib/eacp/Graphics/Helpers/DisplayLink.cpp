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
    rateLimit->setMaxFps(fps);
}

int DisplayLink::maxFps() const
{
    return rateLimit->maxFps();
}

// The divider that implements setMaxFps: a FramePacer fed the real time
// between the link's own ticks.
Callback DisplayLink::rateLimited(const std::shared_ptr<RateLimit>& limit,
                                  const Callback& tick)
{
    using Clock = std::chrono::steady_clock;

    auto lastTick = std::make_shared<Clock::time_point>(Clock::now());

    return [limit, lastTick, tick]
    {
        const auto now = Clock::now();
        const auto period = std::chrono::duration<double>(now - *lastTick).count();
        *lastTick = now;

        if (limit->advance(period))
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

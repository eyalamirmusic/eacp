#include "DisplayLink.h"
#include <algorithm>
#include <chrono>

namespace eacp::Threads
{
DisplayLink::DisplayLink(const Callback& cb)
    : DisplayLink(FrameCallback([cb](FrameTime) { cb(); }))
{
}

// The timing state lives in the returned callback, so the platform Natives stay
// timing-agnostic.
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

        // Clamped so the first frame after a stall is a step, not a jump.
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

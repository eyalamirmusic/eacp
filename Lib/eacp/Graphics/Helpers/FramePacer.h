#pragma once

#include <atomic>

namespace eacp::Threads
{
// The divider behind a frame-rate cap: fed the time between ticks of a clock
// that runs faster than wanted, it says which ticks to act on so they come at
// most `fps` times a second. DisplayLink::setMaxFps is one, and so is every
// UI::GPUComponent's.
//
// It fires on whichever tick lands closest to due: waiting for the next one
// when this one is within half a tick of the target would overshoot by more
// than firing now undershoots. Without that half-tick grace, 60 on a 120Hz
// panel misses the interval by float dust and runs at 40. The remainder is
// carried, so a cap the tick rate does not divide evenly holds as an average,
// and the bank is capped at half an interval, so a stall buys one prompt tick
// rather than a burst of catch-up ones.
//
// The first tick after a start, or after the cap changes from none to some,
// always fires. setMaxFps may be called from any thread; advance from the one
// thread that ticks.
class FramePacer
{
public:
    FramePacer() = default;

    FramePacer(const FramePacer&) = delete;
    FramePacer& operator=(const FramePacer&) = delete;

    // Zero, the default, fires on every tick.
    void setMaxFps(int fps);
    int maxFps() const;

    // Whether to act on this tick, `seconds` after the one before it.
    bool advance(double seconds);

    // The next tick fires, as the first one does.
    void reset();

private:
    std::atomic<int> fps {0};
    std::atomic<double> minInterval {0.0};

    double accumulated = 0.0;
    bool started = false;
};
} // namespace eacp::Threads

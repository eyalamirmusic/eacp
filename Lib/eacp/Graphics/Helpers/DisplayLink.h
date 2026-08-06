#pragma once

#include "../Common.h"

#include <atomic>
#include <memory>

namespace eacp::Threads
{
// Timing for one DisplayLink frame: `time` is seconds since the link started,
// `delta` is seconds since the previous frame (0 on the first frame, clamped
// to 0.1s across stalls so animations step rather than jump). Scale animation
// steps by `delta` so motion speed is independent of refresh rate and
// unaffected by skipped frames.
struct FrameTime
{
    double time = 0.0;
    double delta = 0.0;
};

// Fires a callback on the main thread once per display refresh, synchronized
// with the platform compositor's vsync (CVDisplayLink / CADisplayLink on
// Apple platforms, the DWM compositor clock on Windows).
//
// Keep the callback light (advance state, invalidate); a handler that takes
// as long as a refresh interval keeps the event queue permanently non-empty,
// which starves input processing. Schedule actual rendering through the
// view's repaint path, which yields to pending events.
class DisplayLink
{
public:
    explicit DisplayLink(const Callback& cb);

    using FrameCallback = std::function<void(FrameTime)>;
    explicit DisplayLink(const FrameCallback& cb);

    // Wraps a FrameCallback in a plain Callback that stamps each invocation
    // with a FrameTime (the timing state lives inside the returned
    // callback). For callers that drive frames from their own trigger — a
    // camera frame arriving — with the link's timing semantics.
    static Callback timedTick(const FrameCallback& cb);

    // Caps how often the callback fires. The link still wakes at every
    // refresh — that is all the platform offers — but ticks are skipped so
    // the callback runs at most `fps` times a second: 60 on a 120Hz panel
    // means every second vsync. FrameTime.delta spans the skipped ticks, so
    // delta-scaled animation steps exactly as far as it would on a native
    // 60Hz display. A cap the refresh rate does not divide evenly is paced
    // to the nearest tick and holds as an average. Zero (the default) fires
    // on every refresh. Retune any time; takes effect on the next tick.
    void setMaxFps(int fps);
    int maxFps() const;

private:
    // Shared with the wrapper rateLimited() builds, which runs on the main
    // thread; the atomics only make setMaxFps safe to call while a tick is
    // in flight.
    struct RateLimit
    {
        std::atomic<int> fps {0};
        std::atomic<double> minInterval {0.0};
    };

    // Wraps a tick callback in the divider that skips ticks until `limit`'s
    // interval has accumulated (the pacing state lives inside the returned
    // callback, exactly like timedTick's).
    static Callback rateLimited(const std::shared_ptr<RateLimit>& limit,
                                const Callback& tick);

    std::shared_ptr<RateLimit> rateLimit;
    Callback callback;

    struct Native;
    Pimpl<Native> impl;
};
} // namespace eacp::Threads

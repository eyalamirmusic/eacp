#pragma once

#include "../Common.h"

namespace eacp::Threads
{
// Seconds: `time` since the link started, `delta` since the previous frame
// (0 on the first, clamped to 0.1 across stalls). Scale animation by `delta`.
struct FrameTime
{
    double time = 0.0;
    double delta = 0.0;
};

// Fires on the main thread once per display refresh, synchronized with the
// compositor's vsync. Keep the callback light: a handler as long as a refresh
// interval starves input processing. Render through the view's repaint path.
class DisplayLink
{
public:
    explicit DisplayLink(const Callback& cb);

    using FrameCallback = std::function<void(FrameTime)>;
    explicit DisplayLink(const FrameCallback& cb);

    // Wraps a FrameCallback in a plain Callback that stamps each invocation
    // with a FrameTime, for callers driving frames from their own trigger.
    static Callback timedTick(const FrameCallback& cb);

private:
    Callback callback;

    struct Native;
    Pimpl<Native> impl;
};
} // namespace eacp::Threads

#pragma once

#include <eacp/Core/Utils/Common.h>

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

private:
    static Callback timedTick(const FrameCallback& cb);

    Callback callback;

    struct Native;
    Pimpl<Native> impl;
};
} // namespace eacp::Threads

#pragma once

/** @file Timer, a periodic callback on the main thread. */

#include "../Utils/Common.h"

namespace eacp::Threads
{
/// A repeating callback on the main thread, first fired one interval after
/// construction. Destroying it stops the ticks. Destroying it inside its own
/// callback is undefined; defer that with callAsync. Construct and destroy it
/// on the main thread: debug builds assert, release builds don't check and the
/// timer may never fire. From another thread, construct it inside callAsync.
class Timer
{
public:
    /// Ticks on the message thread every `interval`. The spelling for a period
    /// no whole number of Hz can express - a 41250 ms heartbeat, a 250 ms poll.
    Timer(const Callback& cbToUse, Time::MS interval);
    /// Ticks on the message thread intervalHz times a second.
    Timer(const Callback& cbToUse, int intervalHz);

private:
    Callback callback;

    struct Native;
    Pimpl<Native> impl;

    /// Windows: detachCurrentThreadAsMain kills every live timer through it.
    friend void stopLiveTimers();
};
} // namespace eacp::Threads

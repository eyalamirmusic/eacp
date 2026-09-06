#pragma once

#include "../Utils/Common.h"

namespace eacp::Threads
{
class Timer
{
public:
    // Ticks on the message thread every `interval`. The spelling for a period
    // no whole number of Hz can express - a 41250 ms heartbeat, a 250 ms poll.
    Timer(const Callback& cbToUse, Time::MS interval);
    Timer(const Callback& cbToUse, int intervalHz);

private:
    Callback callback;

    struct Native;
    Pimpl<Native> impl;
};
} // namespace eacp::Threads

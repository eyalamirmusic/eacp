#pragma once

#include "../Utils/Common.h"

namespace eacp::Threads
{
class Timer
{
public:
    Timer(const Callback& cbToUse, int intervalHz);

    // Stops the timer for good — it never fires again. Idempotent, and safe
    // to call from inside the timer's own callback, which destroying the
    // Timer there is not.
    void stop();

private:
    Callback callback;

    struct Native;
    Pimpl<Native> impl;
};
} // namespace eacp::Threads

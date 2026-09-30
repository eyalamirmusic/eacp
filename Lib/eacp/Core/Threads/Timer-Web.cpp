#include "Timer.h"
#include "ThreadUtils.h"

#include <emscripten/eventloop.h>

// The browser's setInterval, which fires on the one thread there is.

namespace eacp::Threads
{
struct Timer::Native
{
    Native(const Callback& cbToUse, double intervalSec)
        : cb(cbToUse)
    {
        assertMainThread();
        assert(intervalSec > 0 && "Timer interval must be positive");

        id = emscripten_set_interval(&Native::tick, intervalSec * 1000.0, this);
    }

    ~Native()
    {
        assertMainThread();
        emscripten_clear_interval(id);
    }

    static void tick(void* data) { static_cast<Native*>(data)->cb(); }

    Callback cb;
    long id = 0;
};

Timer::Timer(const Callback& cbToUse, Time::MS interval)
    : callback(cbToUse)
    , impl(cbToUse, (double) interval.count / 1000.0)
{
}

Timer::Timer(const Callback& cbToUse, int intervalHz)
    : callback(cbToUse)
    , impl(cbToUse, 1.0 / (double) intervalHz)
{
}
} // namespace eacp::Threads

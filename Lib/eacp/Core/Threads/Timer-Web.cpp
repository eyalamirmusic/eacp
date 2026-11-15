#include "Timer.h"
#include "EventLoop.h"

#include <emscripten/em_js.h>
#include <emscripten/emscripten.h>

#include <memory>
#include <utility>

extern "C"
{
    EMSCRIPTEN_KEEPALIVE void eacpWebTimerTick(void* state);
}

// Not emscripten_set_interval, which holds a runtime keepalive: quit()'s
// emscripten_force_exit zeroes the count, so clearing one for a timer
// destroyed with the statics asserts and aborts the exit.
// clang-format off
EM_JS_DEPS(eacpWebTimer, "$callUserCallback");

EM_JS(int, eacpWebStartInterval, (void* state, double intervalMs), {
    var intervals = Module["eacpIntervals"] || (Module["eacpIntervals"] = new Map());
    var id = Module["eacpNextIntervalId"] = (Module["eacpNextIntervalId"] || 0) + 1;
    intervals.set(id, setInterval(function() {
        callUserCallback(function() { _eacpWebTimerTick(state); });
    }, intervalMs));
    return id;
});

EM_JS(void, eacpWebStopInterval, (int id), {
    var intervals = Module["eacpIntervals"];
    clearInterval(intervals.get(id));
    intervals.delete(id);
});
// clang-format on

namespace eacp::Threads
{
namespace
{
struct WebTimerState
{
    Callback cb = [] {};
    bool alive = true;
    bool tickQueued = false;
};

using WebTimerHandle = std::shared_ptr<WebTimerState>;

// One tick queued at a time, and a queued tick can outlive its timer.
void queueTick(const WebTimerHandle& state)
{
    if (std::exchange(state->tickQueued, true))
        return;

    callAsync(
        [held = state]
        {
            held->tickQueued = false;

            if (held->alive)
                held->cb();
        });
}
} // namespace

struct Timer::Native
{
    Native(const Callback& cbToUse, double intervalSec)
        : state(std::make_shared<WebTimerState>(cbToUse))
    {
        assert(intervalSec > 0 && "Timer interval must be positive");
        intervalId = eacpWebStartInterval(&state, intervalSec * 1000.0);
    }

    ~Native()
    {
        state->alive = false;
        eacpWebStopInterval(intervalId);
    }

    Native(const Native&) = delete;
    Native& operator=(const Native&) = delete;

    WebTimerHandle state;
    int intervalId = 0;
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

void eacpWebTimerTick(void* state)
{
    eacp::Threads::queueTick(*static_cast<eacp::Threads::WebTimerHandle*>(state));
}

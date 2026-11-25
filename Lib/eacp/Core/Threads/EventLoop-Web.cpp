#include "EventLoop-Web.h"
#include "CallAfter.h"
#include "EventLoop.h"
#include "ThreadPool.h"
#include "../App/App.h"
#include "../Utils/Console.h"
#include "../Utils/Singleton.h"

#include <emscripten/em_js.h>
#include <emscripten/emscripten.h>
#include <emscripten/eventloop.h>

#include <algorithm>
#include <chrono>
#include <map>
#include <optional>
#include <stdexcept>

// One thread, and the loop is the browser's (or node's): callAsync queues the
// work and schedules one drain on a later turn of that loop. run() does not
// return: it unwinds the caller's stack back to the browser, as an exception
// would, with the runtime kept alive; quit() then does the teardown run<T>()
// does after its loop elsewhere, and exits the runtime.
//
// In an executable that can suspend (EventLoop-Web.h) run() and runFor() are
// real loops instead, which wait for a wake and return as they do elsewhere.
extern "C"
{
    __attribute__((weak)) bool eacpWebCanSuspend()
    {
        return false;
    }

    __attribute__((weak)) void eacpWebSuspendUntilWoken(double) {}

    __attribute__((weak)) void eacpWebWakeSuspended() {}

    EMSCRIPTEN_KEEPALIVE void eacpWebFireCallAfter();
}

// A plain timeout, not emscripten_set_timeout, which holds a runtime keepalive:
// every loop that outlives main() keeps the runtime alive itself, and quit()'s
// emscripten_force_exit zeroes the count, so clearing one during static
// destruction asserts and aborts the exit.
// clang-format off
EM_JS_DEPS(eacpWebCallAfter, "$callUserCallback");

EM_JS(void, eacpWebArmCallAfterTimeout, (double delayMs), {
    clearTimeout(Module["eacpCallAfterTimeout"]);
    Module["eacpCallAfterTimeout"] = setTimeout(function() {
        Module["eacpCallAfterTimeout"] = undefined;
        callUserCallback(_eacpWebFireCallAfter);
    }, delayMs);
});

EM_JS(void, eacpWebDisarmCallAfterTimeout, (), {
    clearTimeout(Module["eacpCallAfterTimeout"]);
    Module["eacpCallAfterTimeout"] = undefined;
});
// clang-format on

namespace eacp::Threads
{
namespace
{
struct WebLoop
{
    Vector<Callback> queue;
    bool drainScheduled = false;
    bool rootRunning = false;
    bool quitRequested = false;
    bool stopRequested = false;

    // The pumps on the main thread's stack, each suspended whenever anything
    // else runs.
    int pumpDepth = 0;
};

WebLoop& getLoop()
{
    return Singleton::get<WebLoop>();
}

bool loopIsRunning(const WebLoop& loop)
{
    return loop.rootRunning || loop.pumpDepth > 0;
}

void drainPending(WebLoop& loop)
{
    auto pending = std::exchange(loop.queue, {});

    for (auto& callback: pending)
        callback();
}

void runScheduledDrain(void*);

// A pump on the stack drains for itself once woken; a callback from the
// browser cannot nest a pump of its own, so it never drains under one.
void scheduleDrain(WebLoop& loop)
{
    if (loop.pumpDepth > 0)
    {
        eacpWebWakeSuspended();
        return;
    }

    if (!std::exchange(loop.drainScheduled, true))
        emscripten_set_immediate(runScheduledDrain, nullptr);
}

void scheduleDrainIfPending(WebLoop& loop)
{
    if (!loop.queue.empty())
        scheduleDrain(loop);
}

// With no loop - main() returned without one, or after quit() - work stays
// queued, as in a queue nothing pumps, until a loop starts.
void runScheduledDrain(void*)
{
    auto& loop = getLoop();
    loop.drainScheduled = false;

    if (loop.pumpDepth > 0)
        eacpWebWakeSuspended();
    else if (loopIsRunning(loop))
        drainPending(loop);
}

struct PumpScope
{
    explicit PumpScope(WebLoop& loopToUse)
        : loop(loopToUse)
    {
        ++loop.pumpDepth;
    }

    ~PumpScope()
    {
        --loop.pumpDepth;
        scheduleDrainIfPending(loop);
    }

    PumpScope(const PumpScope&) = delete;
    PumpScope& operator=(const PumpScope&) = delete;

    WebLoop& loop;
};

void suspendUntilWoken(Time::MS timeout)
{
    eacpWebSuspendUntilWoken(static_cast<double>(timeout.count));
}

bool pumpUntilStoppedOrDue(WebLoop& loop, Time::MS timeout)
{
    auto scope = PumpScope {loop};
    auto deadline = Time::Deadline {timeout};

    while (true)
    {
        drainPending(loop);

        if (loop.quitRequested || std::exchange(loop.stopRequested, false))
            return true;

        if (deadline.expired())
            return false;

        suspendUntilWoken(deadline.remaining());
    }
}

// run<T>()'s teardown after its loop, which run() never returned to.
void finishRootLoop(void*)
{
    Apps::destroyApp();
    stopSharedThreadPool();
    stopCallAfterScheduler();
    emscripten_force_exit(Apps::getReturnValue());
}

[[noreturn]] void refuseNestedPump()
{
    constexpr auto message =
        "Threads::runEventLoopFor: the web's main thread cannot wait for its own "
        "event loop unless the executable links eacp-core-nested-pump";

    Console::printError(message);
    throw std::logic_error {message};
}

using CallAfterClock = std::chrono::steady_clock;

double millisecondsUntil(CallAfterClock::time_point due)
{
    auto left = due - CallAfterClock::now();
    return std::chrono::duration<double, std::milli>(left).count();
}

// One JS timeout, armed for the nearest deadline: a timeout per callback
// would let node fire two close deadlines out of order.
struct CallAfterTimers
{
    CallAfterTimers() = default;
    ~CallAfterTimers() { eacpWebDisarmCallAfterTimeout(); }

    CallAfterTimers(const CallAfterTimers&) = delete;
    CallAfterTimers& operator=(const CallAfterTimers&) = delete;

    std::multimap<CallAfterClock::time_point, Callback> pending;
    std::optional<CallAfterClock::time_point> armedFor;
};

CallAfterTimers& getCallAfterTimers()
{
    return Singleton::get<CallAfterTimers>();
}

void armCallAfterTimeout(CallAfterTimers& timers)
{
    if (timers.pending.empty())
        return;

    auto nearest = timers.pending.begin()->first;

    if (timers.armedFor && *timers.armedFor <= nearest)
        return;

    timers.armedFor = nearest;
    eacpWebArmCallAfterTimeout(std::max(0.0, millisecondsUntil(nearest)));
}

void fireCallAfter()
{
    auto& timers = getCallAfterTimers();
    timers.armedFor.reset();

    while (!timers.pending.empty()
           && timers.pending.begin()->first <= CallAfterClock::now())
    {
        callAsync(timers.pending.begin()->second);
        timers.pending.erase(timers.pending.begin());
    }

    armCallAfterTimeout(timers);
}
} // namespace

void EventLoop::run()
{
    auto& loop = getLoop();
    loop.quitRequested = false;
    loop.rootRunning = true;

    if (!eacpWebCanSuspend())
    {
        scheduleDrainIfPending(loop);
        emscripten_exit_with_live_runtime();
    }

    {
        auto scope = PumpScope {loop};

        while (!loop.quitRequested)
        {
            drainPending(loop);

            if (!loop.quitRequested)
                suspendUntilWoken(Time::MS {-1});
        }
    }

    loop.rootRunning = false;
}

bool EventLoop::runFor(Time::MS timeout)
{
    auto& loop = getLoop();

    if (!eacpWebCanSuspend())
    {
        if (timeout.count > 0)
            refuseNestedPump();

        drainPending(loop);
        return false;
    }

    // A quit made before this pump started is for the pumps beneath it, as on
    // Linux: this one still waits, and they still end once it returns.
    auto quitForPumpsBeneath = loop.pumpDepth > 0 && loop.quitRequested;
    loop.quitRequested = false;
    loop.stopRequested = false;

    auto stopped = pumpUntilStoppedOrDue(loop, timeout);
    loop.quitRequested = loop.quitRequested || quitForPumpsBeneath;
    return stopped;
}

void EventLoop::quit()
{
    auto& loop = getLoop();

    if (eacpWebCanSuspend())
    {
        loop.quitRequested = true;
        scheduleDrain(loop);
        return;
    }

    if (std::exchange(loop.rootRunning, false))
        emscripten_set_immediate(finishRootLoop, nullptr);
}

// Ends the innermost runFor and never the root loop, as on Windows: only
// quit() does that.
void stopEventLoop()
{
    auto& loop = getLoop();
    loop.stopRequested = true;
    scheduleDrain(loop);
}

void EventLoop::call(Callback func)
{
    auto& loop = getLoop();
    loop.queue.add(std::move(func));
    scheduleDrain(loop);
}

void scheduleStartup(const Callback& func)
{
    callAsync(func);
}

void callAfter(Time::MS delay, Callback func)
{
    if (delay.count <= 0)
    {
        callAsync(func);
        return;
    }

    auto& timers = getCallAfterTimers();
    auto due = CallAfterClock::now() + std::chrono::milliseconds {delay.count};
    timers.pending.emplace(due, std::move(func));
    armCallAfterTimeout(timers);
}

void stopCallAfterScheduler()
{
    auto& timers = getCallAfterTimers();
    auto dropped = std::exchange(timers.pending, {});

    eacpWebDisarmCallAfterTimeout();
    timers.armedFor.reset();
}

bool isEventLoopRunning()
{
    return loopIsRunning(getLoop());
}

// A page has no host loop to attach to.
void attachCurrentThreadAsMain() {}

void detachCurrentThreadAsMain()
{
    stopCallAfterScheduler();
    stopSharedThreadPool();
}

// One eacp copy per page: there is no other loop to stop.
void stopProcessRootLoop() {}
} // namespace eacp::Threads

void eacpWebFireCallAfter()
{
    eacp::Threads::fireCallAfter();
}

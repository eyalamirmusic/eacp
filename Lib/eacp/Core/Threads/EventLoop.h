#pragma once

#include "../Utils/Common.h"

namespace eacp::Threads
{
struct EventLoop
{
    void run();
    bool runFor(Time::MS timeout);
    void quit();
    void call(Callback func);
};

EventLoop& getEventLoop();

void runEventLoop(const Callback& func = [] {});
bool runEventLoopFor(Time::MS timeout, const Callback& func = [] {});
void callAsync(const Callback& func);
void stopEventLoop();

// Marks the calling thread as this copy's main/UI thread and brings up what
// callAsync needs, without running a loop. Idempotent; call it once on the
// host's UI thread when eacp is statically linked into a dlopen-hosted plugin.
void attachCurrentThreadAsMain();

// Stops the process root loop, provided some eacp copy is running it — the
// EACP_ROOT_LOOP environment marker crosses DLL boundaries. A loop owned by a
// foreign host carries no marker, so this is a no-op there.
void stopProcessRootLoop();

// True while a loop that callAsync work will reach is running: this copy's root
// loop, a nested runEventLoopFor, or another eacp copy's root loop. False during
// app teardown, where a callAsync would sit in the queue forever.
bool isEventLoopRunning();

// Schedules the app's one-time startup callback. Platform-specific: iOS defers
// it to UIScene connection, everywhere else posts it to the loop immediately.
void scheduleStartup(const Callback& func);

// Pumps in `slice`-sized steps until `ready()` or `timeout`. Returns true if the
// predicate was met, false on timeout. Main thread only, and must not be
// re-entered from inside another event-loop callback.
template <typename Predicate>
bool runEventLoopUntil(Predicate ready,
                       Time::MS timeout,
                       Time::MS slice = Time::MS {20})
{
    if (ready())
        return true;

    auto deadline = Time::Deadline {timeout};

    while (!deadline.expired())
    {
        auto remaining = deadline.remaining();
        runEventLoopFor(slice < remaining ? slice : remaining);

        if (ready())
            return true;
    }

    return ready();
}
} // namespace eacp::Threads

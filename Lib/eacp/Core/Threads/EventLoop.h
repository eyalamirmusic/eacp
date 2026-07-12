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

// Marks the calling thread as this eacp copy's main/UI thread and brings up
// the services callAsync depends on, without running an event loop. For eacp
// statically linked into a dlopen-hosted plugin: the host owns the loop, so
// call this once on the host's UI thread (creating a Window or EmbeddedView
// does it implicitly) and the host's own pump then drives this copy's async
// callbacks and timers. Idempotent. A no-op where the main run loop is a
// process singleton (macOS/Linux) — there callAsync already reaches the
// host's loop without any setup.
void attachCurrentThreadAsMain();

// Stops the process's root run loop, provided an eacp copy is running it —
// any copy: loop ownership is marked in the process environment
// (EACP_ROOT_LOOP), which crosses DLL boundaries. The quit path for an app
// that lives in a dynamic library while a thin host executable pumps the
// loop (Apps::run<T> detects that case and rides the host's loop). A loop
// owned by a foreign host (a DAW) carries no marker, so this is a no-op
// there: that loop is never ours to stop.
void stopProcessRootLoop();

// Schedules the app's one-time startup callback (the app/window creation that
// runEventLoop kicks off). Most platforms post it to the loop immediately; iOS
// defers it to UIScene connection so the window is created with a live scene
// after activation, not before. Platform-specific.
void scheduleStartup(const Callback& func);

// Pumps the event loop in short slices until `ready()` returns true or
// `timeout` elapses. Returns true if the predicate was met, false on
// timeout. Must be called on the main thread, and must not be re-entered
// from inside another event-loop callback.
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

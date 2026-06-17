#pragma once

#include "../Utils/Common.h"
#include <algorithm>
#include <chrono>

namespace eacp::Threads
{
struct EventLoop
{
    void run();
    bool runFor(std::chrono::milliseconds timeout);
    void quit();
    void call(Callback func);
};

EventLoop& getEventLoop();

void runEventLoop(const Callback& func = [] {});
bool runEventLoopFor(
    std::chrono::milliseconds timeout, const Callback& func = [] {});
void callAsync(const Callback& func);
void stopEventLoop();

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
bool runEventLoopUntil(
    Predicate ready,
    std::chrono::milliseconds timeout,
    std::chrono::milliseconds slice = std::chrono::milliseconds(20))
{
    if (ready())
        return true;

    auto deadline = std::chrono::steady_clock::now() + timeout;

    while (true)
    {
        auto now = std::chrono::steady_clock::now();
        if (now >= deadline)
            return ready();

        auto remaining =
            std::chrono::ceil<std::chrono::milliseconds>(deadline - now);

        runEventLoopFor(std::min(slice, remaining));

        if (ready())
            return true;
    }
}
} // namespace eacp::Threads

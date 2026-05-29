#include "EventLoop.h"
#include "../Utils/Singleton.h"

namespace eacp::Threads
{
EventLoop& getEventLoop()
{
    return Singleton::get<EventLoop>();
}

void callAsync(const Callback& func)
{
    getEventLoop().call(func);
}

void runEventLoop(const Callback& func)
{
    callAsync(func);
    getEventLoop().run();
}

bool runEventLoopFor(std::chrono::milliseconds timeout, const Callback& func)
{
    callAsync(func);
    return getEventLoop().runFor(timeout);
}

#if !defined(_WIN32)
// Platforms with CFRunLoop semantics: quit() only affects the innermost
// pump (CFRunLoopStop is reference-counted), so stopEventLoop and
// EventLoop::quit can share an implementation. Windows needs a split —
// stopEventLoop must only unblock the innermost runFor without
// terminating the outer run() — and provides its own override in
// EventLoop-Windows.cpp.
void stopEventLoop()
{
    getEventLoop().quit();
}
#endif
} // namespace eacp::Threads
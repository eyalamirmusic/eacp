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
    scheduleStartup(func);
    getEventLoop().run();
}

bool runEventLoopFor(Time::MS timeout, const Callback& func)
{
    callAsync(func);
    return getEventLoop().runFor(timeout);
}
} // namespace eacp::Threads
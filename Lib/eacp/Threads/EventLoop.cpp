#include "EventLoop.h"
#include "../Utils/Singleton.h"

namespace eacp::Threads
{
EventLoop& getEventLoop()
{ return Singleton::get<EventLoop>(); }

void callAsync(const Callback& func)
{ getEventLoop().call(func); }

void runEventLoop(const Callback& func)
{
    callAsync(func);
    getEventLoop().run();
}

void stopEventLoop()
{ getEventLoop().quit(); }
} // namespace eacp::Threads
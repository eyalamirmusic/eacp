#pragma once

#include <functional>

namespace eacp::Threads
{
using Callback = std::function<void()>;

struct EventLoop
{
    void run();
    void quit();
    void call (Callback func);
};

EventLoop& getEventLoop();

void runEventLoop(const Callback& func = []{});
void callAsync(const Callback& func);
void stopEventLoop();
} // namespace eacp::Threads
#pragma once

#include "../Utils/Common.h"
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
bool runEventLoopFor(std::chrono::milliseconds timeout,
                     const Callback& func = [] {});
void callAsync(const Callback& func);
void stopEventLoop();
} // namespace eacp::Threads

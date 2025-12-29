#pragma once

#include "../Threads/EventLoop.h"

namespace eacp::Apps
{
struct AppBase
{
    virtual ~AppBase() = default;
};

template <typename T>
struct App : AppBase
{
    T app;
};

using AppHandle = std::unique_ptr<AppBase>;

AppHandle& getGlobalApp();

void quit();

template <typename T>
void run()
{
    auto createFunc = [] { getGlobalApp() = std::make_unique<App<T>>(); };
    Threads::runEventLoop(createFunc);
}

} // namespace eacp::Apps
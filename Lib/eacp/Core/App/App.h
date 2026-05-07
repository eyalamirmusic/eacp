#pragma once

#include "../Threads/EventLoop.h"
#include <ea_data_structures/Pointers/OwningPointer.h>

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

using AppHandle = EA::OwningPointer<AppBase>;

AppHandle& getGlobalApp();

void quit();

template <typename T>
void run()
{
    auto createFunc = [] { getGlobalApp().template create<App<T>>(); };
    Threads::runEventLoop(createFunc);
}

} // namespace eacp::Apps
#include "App.h"
#include "AppEnvironment.h"
#include "../Utils/Singleton.h"

namespace eacp::Apps
{
AppHandle& getGlobalApp()
{
    return Singleton::get<AppHandle>();
}

AppFactory& getAppFactory()
{
    return Singleton::get<AppFactory>();
}

AppEnvironment& getAppEnvironment()
{
    return Singleton::get<AppEnvironment>();
}

void setCommandLineArgs(int argc, char* argv[])
{
    auto& args = getAppEnvironment().commandLineArgs;
    args.clear();
    args.reserve(argc);
    for (auto i = 0; i < argc; ++i)
        args.emplace_back(argv[i]);
}

void destroyApp()
{
    getGlobalApp().reset();
}

void quit()
{
    // Destroy synchronously then stop the loop. Going via
    // callAsync would race with the loop's shutdown on Windows
    // (the WM_QUIT outruns any dispatcher-queued work the outer
    // pump never gets to process). When called from inside the
    // app's own constructor the OwningPointer isn't populated
    // yet, so destroyApp() is a no-op there — Apps::run<T> calls
    // it again once the loop has exited to finish the job.
    destroyApp();
    Threads::getEventLoop().quit();
}

void restart()
{
    auto restartFunc = []
    {
        destroyApp();

        auto& factory = getAppFactory();
        if (factory)
            factory();
    };

    Threads::callAsync(restartFunc);
}
} // namespace eacp::Apps
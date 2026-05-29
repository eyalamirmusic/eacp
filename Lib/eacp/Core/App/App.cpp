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

static void quitSync()
{
    destroyApp();
    Threads::getEventLoop().quit();
}

void quit()
{
    Threads::callAsync(quitSync);
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
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

void setHeadless(bool headless)
{
    getAppEnvironment().headless = headless;
    setEnv("EACP_HEADLESS", headless ? "1" : "0");
}

void destroyApp()
{
    getGlobalApp().reset();
}

// Quitting only stops the loop. The app is destroyed by run<T>() after the
// loop has fully unwound, never from inside a runloop callback: a callback
// can fire from a nested native pump (a window resize/drag tracking loop, a
// modal), and destroying views while the platform is mid-event-delivery on
// them is a use-after-free.
static void quitSync()
{
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
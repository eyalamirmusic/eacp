#include "App.h"
#include "AppEnvironment.h"
#include "../Utils/Singleton.h"

#include <atomic>

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

namespace
{
bool s_runningAsPlugin = false;
std::atomic<int> s_returnValue {0};
Callback s_reopenHandler = [] {};
} // namespace

void setReopenHandler(const Callback& handler)
{
    s_reopenHandler = handler ? handler : Callback {[] {}};
}

const Callback& getReopenHandler()
{
    return s_reopenHandler;
}

void setReturnValue(int returnValue)
{
    s_returnValue = returnValue;
}

int getReturnValue()
{
    return s_returnValue;
}

bool isRunningAsPlugin()
{
    return s_runningAsPlugin;
}

void Detail::runAsPlugin(const AppFactory& createFunc)
{
    s_runningAsPlugin = true;
    Threads::scheduleStartup(createFunc);
}

// Only stops the loop; run<T>() destroys the app once the loop has unwound.
static void quitSync()
{
    // A plugin-hosted app has no loop of its own. Under a foreign host
    // stopProcessRootLoop is a no-op: quitting is the host's decision.
    if (isRunningAsPlugin())
        Threads::stopProcessRootLoop();

    Threads::getEventLoop().quit();
}

void quit()
{
    Threads::callAsync(quitSync);
}

void quit(int returnValue)
{
    setReturnValue(returnValue);
    quit();
}

int run(const Callback& func)
{
    setReturnValue(0);
    Threads::runEventLoop(
        [&func]
        {
            func();
            quit();
        });

    return getReturnValue();
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
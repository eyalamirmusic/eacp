// Fixture for RootLoopTests-Linux: a plugin whose own eacp copy defers work
// the way a real one does — callAsync, callAfter, a Timer and an
// Apps::run<T> app — so the test can watch an eacp host pump it. The static
// initializer counts genuine loads in the process environment, exactly as
// DynamicLibraryTestPlugin does, so the unload cases observe real unmaps.
#include <eacp/Core/App/App.h>
#include <eacp/Core/Plugins/PluginExport.h>
#include <eacp/Core/Threads/EventLoop-Linux.h>
#include <eacp/Core/Threads/Timer.h>
#include <eacp/Core/Utils/Environment.h>
#include <eacp/Core/Utils/Strings.h>

#include <optional>
#include <string>

namespace
{
int bumpLoadCount()
{
    const auto name = "EACP_ROOT_LOOP_PLUGIN_LOADS";
    const auto loads = eacp::Strings::parseIntOr(eacp::getEnvValue(name));

    eacp::setEnv(name, std::to_string(loads + 1));

    return 0;
}

[[maybe_unused]] const auto loadStamp = bumpLoadCount();

std::optional<eacp::Threads::Timer>& pluginTimer()
{
    static auto timer = std::optional<eacp::Threads::Timer> {};
    return timer;
}

// Constructed on the loop that hosts this copy, which is the whole point:
// the counter only moves once something pumped us.
struct PluginApp
{
    PluginApp(int* counter, int quitWhenConstructed)
    {
        ++*counter;

        if (quitWhenConstructed != 0)
            eacp::Apps::quit();
    }
};
} // namespace

EACP_PLUGIN_EXPORT void eacpRootLoopPluginAttach()
{
    eacp::Threads::attachCurrentThreadAsMain();
}

EACP_PLUGIN_EXPORT int eacpRootLoopPluginIsEventLoopRunning()
{
    return eacp::Threads::isEventLoopRunning() ? 1 : 0;
}

EACP_PLUGIN_EXPORT int eacpRootLoopPluginLoopFd()
{
    return eacp::Threads::getEventLoopFd();
}

EACP_PLUGIN_EXPORT void eacpRootLoopPluginPump()
{
    eacp::Threads::pumpEventLoop();
}

EACP_PLUGIN_EXPORT void eacpRootLoopPluginCallAsync(int* counter)
{
    eacp::Threads::callAsync([counter] { ++*counter; });
}

EACP_PLUGIN_EXPORT void eacpRootLoopPluginCallAfter(int* counter, int delayMs)
{
    eacp::Threads::callAfter(eacp::Time::MS {delayMs}, [counter] { ++*counter; });
}

EACP_PLUGIN_EXPORT void eacpRootLoopPluginStartTimer(int* counter, int intervalMs)
{
    pluginTimer().emplace([counter] { ++*counter; }, eacp::Time::MS {intervalMs});
}

EACP_PLUGIN_EXPORT void eacpRootLoopPluginRunApp(int* counter,
                                                 int quitWhenConstructed)
{
    eacp::Apps::run<PluginApp>(counter, quitWhenConstructed);
}

// Everything this copy owns, destroyed while its image is still mapped —
// the quiesce step Plugins::unload names.
EACP_PLUGIN_EXPORT void eacpRootLoopPluginShutdown()
{
    pluginTimer().reset();
    eacp::Apps::destroyApp();
}

#include "Common.h"

#include <eacp/Core/Plugins/DynamicLibrary.h>
#include <eacp/Core/Threads/EventLoop-Linux.h>
#include <eacp/Core/Utils/Environment.h>
#include <eacp/Core/Utils/Strings.h>

#include <poll.h>

using namespace nano;
using namespace eacp;

namespace
{
int pluginLoadCount()
{
    return Strings::parseIntOr(getEnvValue("EACP_ROOT_LOOP_PLUGIN_LOADS"));
}

// This test binary plays the thin eacp host: it runs the process's root loop
// and does nothing else for the plugin, which finds that loop by itself.
struct RootLoopPlugin
{
    RootLoopPlugin()
        : library(EACP_ROOT_LOOP_TEST_PLUGIN)
    {
        attach = library.findFunction<void (*)()>("eacpRootLoopPluginAttach");
        isEventLoopRunning =
            library.findFunction<int (*)()>("eacpRootLoopPluginIsEventLoopRunning");
        loopFd = library.findFunction<int (*)()>("eacpRootLoopPluginLoopFd");
        pump = library.findFunction<void (*)()>("eacpRootLoopPluginPump");
        callAsync =
            library.findFunction<void (*)(int*)>("eacpRootLoopPluginCallAsync");
        callAfter =
            library.findFunction<void (*)(int*, int)>("eacpRootLoopPluginCallAfter");
        startTimer = library.findFunction<void (*)(int*, int)>(
            "eacpRootLoopPluginStartTimer");
        runApp =
            library.findFunction<void (*)(int*, int)>("eacpRootLoopPluginRunApp");
        shutdown = library.findFunction<void (*)()>("eacpRootLoopPluginShutdown");
    }

    bool isLoaded() const
    {
        return library.isOpen() && attach != nullptr && isEventLoopRunning != nullptr
               && loopFd != nullptr && pump != nullptr && callAsync != nullptr
               && callAfter != nullptr && startTimer != nullptr && runApp != nullptr
               && shutdown != nullptr;
    }

    Plugins::DynamicLibrary library;
    void (*attach)() = nullptr;
    int (*isEventLoopRunning)() = nullptr;
    int (*loopFd)() = nullptr;
    void (*pump)() = nullptr;
    void (*callAsync)(int*) = nullptr;
    void (*callAfter)(int*, int) = nullptr;
    void (*startTimer)(int*, int) = nullptr;
    void (*runApp)(int*, int) = nullptr;
    void (*shutdown)() = nullptr;
};

bool isReadable(int fd)
{
    auto fds = pollfd {fd, POLLIN, 0};
    return ::poll(&fds, 1, 0) > 0;
}
} // namespace

// The three ways a hosted copy defers work, all of them reaching the root
// loop of a copy that never heard of the plugin.
auto tHostedWorkRunsUnderTheRootLoop =
    test("RootLoop/hostedWorkRunsUnderTheRootLoop") = []
{
    auto plugin = RootLoopPlugin {};
    check(plugin.isLoaded());

    auto asyncCalls = 0;
    auto delayedCalls = 0;
    auto ticks = 0;

    auto start = [&]
    {
        plugin.attach();
        plugin.callAsync(&asyncCalls);
        plugin.callAfter(&delayedCalls, 20);
        plugin.startTimer(&ticks, 10);
    };

    Threads::callAsync(start);

    check(Threads::runEventLoopUntil(
        [&] { return asyncCalls > 0 && delayedCalls > 0 && ticks >= 3; },
        Time::MS {5000}));

    plugin.shutdown();
};

// attachCurrentThreadAsMain is not the only way in: the first callAsync of a
// copy that never attached explicitly finds the root loop too.
auto tFirstCallAsyncFindsTheRootLoop =
    test("RootLoop/firstCallAsyncFindsTheRootLoop") = []
{
    auto plugin = RootLoopPlugin {};
    check(plugin.isLoaded());

    auto asyncCalls = 0;

    Threads::callAsync([&] { plugin.callAsync(&asyncCalls); });

    check(Threads::runEventLoopUntil([&] { return asyncCalls == 1; },
                                     Time::MS {5000}));

    plugin.shutdown();
};

auto tHostedCopySeesTheRootLoopAsRunning =
    test("RootLoop/hostedCopySeesTheRootLoopAsRunning") = []
{
    auto plugin = RootLoopPlugin {};
    check(plugin.isLoaded());

    check(plugin.isEventLoopRunning() == 0);

    auto insideLoop = 0;
    Threads::runEventLoopFor(Time::MS {50},
                             [&] { insideLoop = plugin.isEventLoopRunning(); });

    check(insideLoop == 1);
};

auto tPluginAppConstructsUnderTheRootLoop =
    test("RootLoop/pluginAppConstructsUnderTheRootLoop") = []
{
    auto plugin = RootLoopPlugin {};
    check(plugin.isLoaded());

    auto constructed = 0;

    Threads::callAsync([&] { plugin.runApp(&constructed, 0); });

    check(Threads::runEventLoopUntil([&] { return constructed == 1; },
                                     Time::MS {5000}));

    plugin.shutdown();
};

// The quit path a hosted Apps::run<T> app takes: its own loop never ran, so
// Apps::quit stops the host's.
auto tPluginAppQuitStopsTheRootLoop =
    test("RootLoop/pluginAppQuitStopsTheRootLoop") = []
{
    auto plugin = RootLoopPlugin {};
    check(plugin.isLoaded());

    auto constructed = 0;

    auto budget = Time::Deadline {Time::MS {5000}};
    auto stopped = Threads::runEventLoopFor(Time::MS {5000},
                                            [&] { plugin.runApp(&constructed, 1); });

    check(constructed == 1);
    check(stopped);
    check(!budget.expired());

    plugin.shutdown();
};

auto tMarkerIsClearedWhenTheRootLoopExits =
    test("RootLoop/markerIsClearedWhenTheRootLoopExits") = []
{
    check(getEnvValue("EACP_ROOT_LOOP") != "1");
    check(getEnv("EACP_ROOT_LOOP_BRIDGE") == std::nullopt);

    auto markerInsideLoop = std::string {};
    auto bridgeInsideLoop = std::string {};

    Threads::runEventLoopFor(Time::MS {50},
                             [&]
                             {
                                 markerInsideLoop = getEnvValue("EACP_ROOT_LOOP");
                                 bridgeInsideLoop =
                                     getEnvValue("EACP_ROOT_LOOP_BRIDGE");
                             });

    check(markerInsideLoop == "1");
    check(bridgeInsideLoop.find(':') != std::string::npos);

    check(getEnvValue("EACP_ROOT_LOOP") == "0");
    check(getEnv("EACP_ROOT_LOOP_BRIDGE") == std::nullopt);
};

// The foreign-host shape, unchanged: with nothing advertising a root loop,
// the plugin's own descriptor and pump are the only things that move it.
auto tWithNoRootLoopNothingAttaches =
    test("RootLoop/withNoRootLoopNothingAttaches") = []
{
    check(!Threads::isEventLoopRunning());

    auto plugin = RootLoopPlugin {};
    check(plugin.isLoaded());

    plugin.attach();
    check(plugin.isEventLoopRunning() == 1);

    auto asyncCalls = 0;
    plugin.callAsync(&asyncCalls);

    Time::sleepMS(50);
    check(asyncCalls == 0);

    check(isReadable(plugin.loopFd()));
    plugin.pump();

    check(asyncCalls == 1);
    check(!isReadable(plugin.loopFd()));

    plugin.shutdown();
};

// The root copy holds a function pointer into the plugin's image, so the
// plugin has to be off the root's poll set before the image goes. Repeated,
// because a stale source is a use-after-unmap the first turn after it.
auto tUnloadDetachesTheHostedLoop = test("RootLoop/unloadDetachesTheHostedLoop") = []
{
    for (auto attempt = 0; attempt < 20; ++attempt)
    {
        setEnv("EACP_ROOT_LOOP_PLUGIN_LOADS", "0");

        auto plugin = RootLoopPlugin {};
        check(plugin.isLoaded());

        auto ticks = 0;
        Threads::callAsync(
            [&]
            {
                plugin.attach();
                plugin.startTimer(&ticks, 5);
            });

        check(
            Threads::runEventLoopUntil([&] { return ticks >= 2; }, Time::MS {2000}));

        auto shutdown = plugin.shutdown;

        Threads::runEventLoopFor(Time::MS {150},
                                 [&]
                                 {
                                     Plugins::unload(std::move(plugin.library),
                                                     [shutdown] { shutdown(); });
                                 });

        auto afterUnload = 0;
        Threads::callAsync([&] { ++afterUnload; });
        check(Threads::runEventLoopUntil([&] { return afterUnload == 1; },
                                         Time::MS {1000}));

        // Reopening re-runs the plugin's static initializers, which is what
        // proves the deferred close really unmapped it.
        auto reopened = Plugins::DynamicLibrary(EACP_ROOT_LOOP_TEST_PLUGIN);
        check(reopened.isOpen());
        check(pluginLoadCount() == 2);
    }
};

auto tBareCloseDetachesTheHostedLoop =
    test("RootLoop/bareCloseDetachesTheHostedLoop") = []
{
    for (auto attempt = 0; attempt < 20; ++attempt)
    {
        setEnv("EACP_ROOT_LOOP_PLUGIN_LOADS", "0");

        auto plugin = RootLoopPlugin {};
        check(plugin.isLoaded());

        auto ticks = 0;
        Threads::callAsync(
            [&]
            {
                plugin.attach();
                plugin.startTimer(&ticks, 5);
            });

        check(
            Threads::runEventLoopUntil([&] { return ticks >= 2; }, Time::MS {2000}));

        // dlclose on the spot, from inside a loop turn, which is what
        // PluginHost does.
        Threads::runEventLoopFor(Time::MS {150},
                                 [&]
                                 {
                                     plugin.shutdown();
                                     plugin.library.close();
                                 });

        check(pluginLoadCount() == 1);

        auto afterUnload = 0;
        Threads::callAsync([&] { ++afterUnload; });
        check(Threads::runEventLoopUntil([&] { return afterUnload == 1; },
                                         Time::MS {1000}));

        auto reopened = Plugins::DynamicLibrary(EACP_ROOT_LOOP_TEST_PLUGIN);
        check(reopened.isOpen());
        check(pluginLoadCount() == 2);
    }
};

// Two copies of the same image are one load and one hosted loop; the last
// close is what detaches it.
auto tTwoHostedCopiesShareOneAttachment =
    test("RootLoop/twoHostedCopiesShareOneAttachment") = []
{
    setEnv("EACP_ROOT_LOOP_PLUGIN_LOADS", "0");

    auto first = RootLoopPlugin {};
    auto second = RootLoopPlugin {};
    check(first.isLoaded());
    check(second.isLoaded());
    check(pluginLoadCount() == 1);

    auto asyncCalls = 0;
    Threads::callAsync(
        [&]
        {
            first.attach();
            second.callAsync(&asyncCalls);
        });

    check(Threads::runEventLoopUntil([&] { return asyncCalls == 1; },
                                     Time::MS {2000}));

    first.shutdown();
    first.library.close();

    auto afterFirstClose = 0;
    Threads::callAsync([&] { second.callAsync(&afterFirstClose); });
    check(Threads::runEventLoopUntil([&] { return afterFirstClose == 1; },
                                     Time::MS {2000}));

    second.library.close();

    auto stillLooping = 0;
    Threads::callAsync([&] { ++stillLooping; });
    check(Threads::runEventLoopUntil([&] { return stillLooping == 1; },
                                     Time::MS {1000}));
};

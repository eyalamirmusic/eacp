#include "Common.h"
#include <eacp/Core/Plugins/DynamicLibrary.h>
#include <eacp/Core/Threads/EventLoop.h>
#include <eacp/Core/Utils/Environment.h>

using namespace nano;
using namespace eacp;

namespace
{
// Bumped by the fixture plugin's static initializer on every genuine load, so
// it counts real map/unmap cycles.
int pluginLoadCount()
{
    auto value = getEnvValue("EACP_TEST_PLUGIN_LOADS");
    return value.empty() ? 0 : std::stoi(value);
}
} // namespace

auto tSharedUntilLastClose = test("DynamicLibrary/sharedUntilLastClose") = []
{
    setEnv("EACP_TEST_PLUGIN_LOADS", "0");

    auto first = Plugins::DynamicLibrary(EACP_TEST_PLUGIN);
    auto second = Plugins::DynamicLibrary(EACP_TEST_PLUGIN);
    check(first.isOpen());
    check(second.isOpen());
    check(pluginLoadCount() == 1);

    auto add = second.findFunction<int (*)(int, int)>("eacpTestPluginAdd");
    check(add != nullptr);
    check(add(2, 3) == 5);

    first.close();
    check(add(4, 4) == 8);

    second.close();
    auto reopened = Plugins::DynamicLibrary(EACP_TEST_PLUGIN);
    check(reopened.isOpen());
    check(pluginLoadCount() == 2);
};

auto tMoveKeepsTheImage = test("DynamicLibrary/moveKeepsTheImage") = []
{
    setEnv("EACP_TEST_PLUGIN_LOADS", "0");

    auto library = Plugins::DynamicLibrary(EACP_TEST_PLUGIN);
    auto moved = std::move(library);
    check(moved.isOpen());
    check(!library.isOpen());

    moved.close();
    auto reopened = Plugins::DynamicLibrary(EACP_TEST_PLUGIN);
    check(pluginLoadCount() == 2);
};

auto tOpenFailure = test("DynamicLibrary/openFailure") = []
{
    auto library = Plugins::DynamicLibrary(FilePath {"no/such/library.so"});
    check(!library.isOpen());
    check(library.findSymbol("anything") == nullptr);
};

auto tUnloadWithNoLoop = test("DynamicLibrary/unloadWithNoLoop") = []
{
    setEnv("EACP_TEST_PLUGIN_LOADS", "0");
    check(!Threads::isEventLoopRunning());

    auto quiesced = false;

    {
        auto library = Plugins::DynamicLibrary(EACP_TEST_PLUGIN);
        check(library.isOpen());

        Plugins::unload(std::move(library), [&quiesced] { quiesced = true; });
        check(!library.isOpen());
    }

    check(quiesced);

    auto reopened = Plugins::DynamicLibrary(EACP_TEST_PLUGIN);
    check(pluginLoadCount() == 2);
};

auto tUnloadDefersWhileLooping = test("DynamicLibrary/unloadDefersWhileLooping") = []
{
    setEnv("EACP_TEST_PLUGIN_LOADS", "0");

    auto library = Plugins::DynamicLibrary(EACP_TEST_PLUGIN);
    auto add = library.findFunction<int (*)(int, int)>("eacpTestPluginAdd");
    check(add != nullptr);

    auto stillMappedInsideTurn = false;

    // Runs inside a loop turn, so unload takes its deferred path.
    Threads::runEventLoopFor(Time::MS {200},
                             [&]
                             {
                                 check(Threads::isEventLoopRunning());
                                 Plugins::unload(std::move(library));

                                 stillMappedInsideTurn = add(2, 3) == 5;
                             });

    check(stillMappedInsideTurn);

    auto reopened = Plugins::DynamicLibrary(EACP_TEST_PLUGIN);
    check(pluginLoadCount() == 2);
};

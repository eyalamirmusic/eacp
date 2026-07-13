#include "Common.h"
#include <eacp/Core/Plugins/DynamicLibrary.h>
#include <eacp/Core/Threads/EventLoop.h>
#include <eacp/Core/Utils/Environment.h>

using namespace nano;
using namespace eacp;

namespace
{
// Bumped by the fixture plugin's static initializer on every genuine load
// (see DynamicLibraryTestPlugin.cpp), so it counts real map/unmap cycles.
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

    // The image survives the first close: `second` still holds it.
    first.close();
    check(add(4, 4) == 8);

    // The last close unloads for real — a fresh open re-runs the plugin's
    // static initializers.
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

    // The moved-from instance's destruction must not drop the reference:
    // closing `moved` is what unloads.
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

// With no loop to defer to, unload runs the quiesce callback and unmaps
// before it returns — the app-teardown path.
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

    // Unmapped already: reopening re-runs the plugin's static initializers.
    auto reopened = Plugins::DynamicLibrary(EACP_TEST_PLUGIN);
    check(pluginLoadCount() == 2);
};

// With a loop running, the unmap is deferred to a later turn — the module's
// code must outlive the turn its teardown ran in.
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

                                 // Same turn: the image is still mapped, so
                                 // calling into it is safe.
                                 stillMappedInsideTurn = add(2, 3) == 5;
                             });

    check(stillMappedInsideTurn);

    // The loop has since run the deferred close, so this is a genuine reload.
    auto reopened = Plugins::DynamicLibrary(EACP_TEST_PLUGIN);
    check(pluginLoadCount() == 2);
};

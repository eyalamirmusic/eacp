#include "Common.h"

using namespace nano;

namespace Connectivity = eacp::Network::Connectivity;

namespace
{
Connectivity::State stateWith(bool online, Connectivity::Interface interface)
{
    auto state = Connectivity::State {};

    state.online = online;
    state.interfaceKind = interface;

    return state;
}
} // namespace

// The whole contract of the first call: it answers, it does not hang, and it
// starts the platform monitor on the way. Which answer it gives depends on
// the machine the suite runs on.
auto tConnectivityAnswersWithoutHanging =
    test("Connectivity/answersWithoutHanging") = []
{
    auto online = Connectivity::isThisMachineOnline();

    check(Connectivity::getState().online == online);
    check(Connectivity::isThisMachineOnline() == online);
};

auto tConnectivityAlwaysNamesTheInterface =
    test("Connectivity/alwaysNamesTheInterface") = []
{
    const auto& state = Connectivity::getState();

    if (!state.online)
        check(state.interfaceKind == Connectivity::Interface::None);

    check(!Connectivity::toString(state.interfaceKind).empty());
};

// The listener sees the state it joined without waiting for a change - which
// is what EA::Listener's default TriggerNow mode is for, and what saves every
// subscriber from an is-it-up-yet call of its own.
auto tConnectivityListenerStartsFromTheCurrentState =
    test("Connectivity/listenerStartsFromTheCurrentState") = []
{
    auto& monitor = Connectivity::Monitor::get();
    auto calls = 0;

    auto listener = EA::Listener {monitor, [&] { ++calls; }};

    check(calls == 1);
};

auto tConnectivityChangeTriggersTheListener =
    test("Connectivity/changeTriggersTheListener") = []
{
    auto& monitor = Connectivity::Monitor::get();
    auto seen = Connectivity::State {};

    auto listener = EA::Listener {monitor, [&] { seen = monitor.getState(); }};

    monitor.setState(stateWith(true, Connectivity::Interface::Wired));

    check(seen.online);
    check(seen.interfaceKind == Connectivity::Interface::Wired);

    monitor.setState(stateWith(false, Connectivity::Interface::None));

    check(!seen.online);
    check(seen.interfaceKind == Connectivity::Interface::None);
};

// A report that says what the monitor already holds is not a change, and a
// listener that redraws on every trigger must not be woken by one.
auto tConnectivityRepeatedStateDoesNotTrigger =
    test("Connectivity/repeatedStateDoesNotTrigger") = []
{
    auto& monitor = Connectivity::Monitor::get();
    auto wired = stateWith(true, Connectivity::Interface::Wired);

    monitor.setState(wired);

    auto calls = 0;
    auto listener = EA::Listener {
        monitor, [&] { ++calls; }, EA::Listener::Modes::TriggerOnEvent};

    monitor.setState(wired);
    check(calls == 0);

    monitor.setState(stateWith(true, Connectivity::Interface::Wifi));
    check(calls == 1);
};

auto tConnectivityListenerStopsWhenDestroyed =
    test("Connectivity/listenerStopsWhenDestroyed") = []
{
    auto& monitor = Connectivity::Monitor::get();
    auto calls = 0;

    {
        auto listener = EA::Listener {
            monitor, [&] { ++calls; }, EA::Listener::Modes::TriggerOnEvent};

        monitor.setState(stateWith(true, Connectivity::Interface::Wired));
        check(calls == 1);
    }

    monitor.setState(stateWith(true, Connectivity::Interface::Wifi));
    check(calls == 1);
};

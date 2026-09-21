#include "Common.h"
#include <atomic>
#include <functional>

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

// The probe, against a server of the test's own. Every case below sets the
// state by hand and stops the probe on the way out, so the platform monitor
// and the cases above see nothing of it.
namespace
{
using eacp::HTTP::Request;
using eacp::HTTP::Response;
using eacp::HTTP::Server;
using eacp::Threads::runEventLoopUntil;
using eacp::Time::MS;

constexpr auto probeTimeout = MS {5000};
constexpr auto probeBody = "eacp-probe-ok";

struct ProbeServer
{
    ProbeServer()
    {
        auto handler = [this](const Request&)
        {
            auto response = Response();
            response.statusCode = 200;
            response.content = body.load() ? probeBody : "<html>log in</html>";

            ++hits;
            return response;
        };

        listening = server.listen(0, handler);
    }

    ~ProbeServer() { server.stop(); }

    std::string url() const
    {
        return "http://127.0.0.1:" + std::to_string(server.boundPort()) + "/probe";
    }

    Connectivity::ProbeOptions options() const
    {
        auto probe = Connectivity::ProbeOptions {};

        probe.url = url();
        probe.expectedContent = probeBody;
        probe.interval = MS {50};
        probe.retryInterval = MS {50};
        probe.timeout = MS {2000};

        return probe;
    }

    Server server;
    std::atomic<bool> body {true};
    std::atomic<int> hits {0};
    bool listening = false;
};

struct StopProbeOnExit
{
    ~StopProbeOnExit() { Connectivity::Monitor::get().stopProbe(); }
};

bool pumpUntil(const std::function<bool()>& ready)
{
    return runEventLoopUntil(ready, probeTimeout);
}
} // namespace

auto tConnectivityReachableMirrorsOnlineWithoutAProbe =
    test("Connectivity/reachableMirrorsOnlineWithoutAProbe") = []
{
    auto& monitor = Connectivity::Monitor::get();
    monitor.stopProbe();

    monitor.setState(stateWith(true, Connectivity::Interface::Wired));
    check(monitor.getState().reachable);
    check(monitor.getState().hasInternet());

    monitor.setState(stateWith(false, Connectivity::Interface::None));
    check(!monitor.getState().reachable);
    check(!Connectivity::hasInternet());
};

auto tConnectivityProbeAssumesReachableUntilAnswered =
    test("Connectivity/probeAssumesReachableUntilAnswered") = []
{
    auto& monitor = Connectivity::Monitor::get();
    auto guard = StopProbeOnExit {};

    monitor.setState(stateWith(true, Connectivity::Interface::Wired));

    auto probe = Connectivity::ProbeOptions {};
    probe.url = "http://127.0.0.1:9/never";
    probe.interval = MS {60000};
    probe.retryInterval = MS {60000};

    monitor.startProbe(probe);

    check(monitor.isProbing());
    check(monitor.getState().reachable);
};

auto tConnectivityProbeAgainstAnAnsweringServer =
    test("Connectivity/probeAgainstAnAnsweringServer") = []
{
    auto server = ProbeServer {};
    check(server.listening);

    auto& monitor = Connectivity::Monitor::get();
    auto guard = StopProbeOnExit {};

    monitor.setState(stateWith(true, Connectivity::Interface::Wired));
    monitor.startProbe(server.options());

    check(pumpUntil([&] { return server.hits.load() >= 2; }));
    check(monitor.getState().reachable);
    check(monitor.getState().hasInternet());
};

auto tConnectivityProbeClearsReachableWhenTheAnswerIsWrong =
    test("Connectivity/probeClearsReachableWhenTheAnswerIsWrong") = []
{
    auto server = ProbeServer {};
    check(server.listening);
    server.body.store(false);

    auto& monitor = Connectivity::Monitor::get();
    auto guard = StopProbeOnExit {};
    auto triggers = 0;

    auto listener = EA::Listener {
        monitor, [&] { ++triggers; }, EA::Listener::Modes::TriggerOnEvent};

    monitor.setState(stateWith(true, Connectivity::Interface::Wired));
    monitor.startProbe(server.options());

    check(pumpUntil([&] { return !monitor.getState().reachable; }));
    check(monitor.getState().online);
    check(!monitor.getState().hasInternet());
    check(triggers >= 1);

    server.body.store(true);

    check(pumpUntil([&] { return monitor.getState().reachable; }));
    check(monitor.getState().hasInternet());
};

auto tConnectivityProbeClearsReachableWhenNothingAnswers =
    test("Connectivity/probeClearsReachableWhenNothingAnswers") = []
{
    auto& monitor = Connectivity::Monitor::get();
    auto guard = StopProbeOnExit {};

    monitor.setState(stateWith(true, Connectivity::Interface::Wired));

    auto server = ProbeServer {};
    check(server.listening);

    auto probe = server.options();
    server.server.stop();

    monitor.startProbe(probe);

    check(pumpUntil([&] { return !monitor.getState().reachable; }));
    check(monitor.getState().online);
};

auto tConnectivityStoppingTheProbeRestoresTheMirror =
    test("Connectivity/stoppingTheProbeRestoresTheMirror") = []
{
    auto server = ProbeServer {};
    check(server.listening);
    server.body.store(false);

    auto& monitor = Connectivity::Monitor::get();

    monitor.setState(stateWith(true, Connectivity::Interface::Wired));
    monitor.startProbe(server.options());

    check(pumpUntil([&] { return !monitor.getState().reachable; }));

    monitor.stopProbe();

    check(!monitor.isProbing());
    check(monitor.getState().reachable);

    auto hitsAtStop = server.hits.load();
    runEventLoopUntil([] { return false; }, MS {200});
    check(server.hits.load() == hitsAtStop);
};

auto tConnectivityProbeWaitsWhileOffline =
    test("Connectivity/probeWaitsWhileOffline") = []
{
    auto server = ProbeServer {};
    check(server.listening);

    auto& monitor = Connectivity::Monitor::get();
    auto guard = StopProbeOnExit {};

    monitor.setState(stateWith(false, Connectivity::Interface::None));
    monitor.startProbe(server.options());

    runEventLoopUntil([] { return false; }, MS {200});
    check(server.hits.load() == 0);
    check(!monitor.getState().reachable);

    monitor.setState(stateWith(true, Connectivity::Interface::Wired));

    check(pumpUntil([&] { return server.hits.load() >= 1; }));
    check(pumpUntil([&] { return monitor.getState().reachable; }));
};

auto tConnectivityProbeNowSkipsTheWait =
    test("Connectivity/probeNowSkipsTheWait") = []
{
    auto server = ProbeServer {};
    check(server.listening);

    auto& monitor = Connectivity::Monitor::get();
    auto guard = StopProbeOnExit {};

    monitor.setState(stateWith(true, Connectivity::Interface::Wired));

    auto probe = server.options();
    probe.interval = MS {60000};
    monitor.startProbe(probe);

    check(pumpUntil([&] { return server.hits.load() == 1; }));

    monitor.probeNow();
    check(pumpUntil([&] { return server.hits.load() == 2; }));
};

// The interval counts from the on-demand fetch, not from the one before it:
// the fetch the schedule had pending is dropped, or a focus-triggered check
// would be followed by a second one moments later.
auto tConnectivityProbeNowRestartsTheInterval =
    test("Connectivity/probeNowRestartsTheInterval") = []
{
    auto server = ProbeServer {};
    check(server.listening);

    auto& monitor = Connectivity::Monitor::get();
    auto guard = StopProbeOnExit {};

    monitor.setState(stateWith(true, Connectivity::Interface::Wired));

    auto probe = server.options();
    probe.interval = MS {800};
    monitor.startProbe(probe);

    check(pumpUntil([&] { return server.hits.load() == 1; }));

    runEventLoopUntil([] { return false; }, MS {400});
    monitor.probeNow();
    check(pumpUntil([&] { return server.hits.load() == 2; }));

    // Past where the original schedule would have fired, short of the new one.
    runEventLoopUntil([] { return false; }, MS {600});
    check(server.hits.load() == 2);

    check(pumpUntil([&] { return server.hits.load() == 3; }));
};

#include "Common.h"
#include <atomic>
#include <functional>
#include <optional>

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

// Everything from here down drives a monitor of the test's own rather than
// the process singleton. CI runs each case in a process of its own, where
// the platform monitor has only just started: its first report lands
// asynchronously, in the middle of any case that pumps the loop, and on a
// wired runner it seeds the very state a case would set to see a change.
auto tConnectivityChangeTriggersTheListener =
    test("Connectivity/changeTriggersTheListener") = []
{
    auto monitor = Connectivity::Monitor {};
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
    auto monitor = Connectivity::Monitor {};
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
    auto monitor = Connectivity::Monitor {};
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

// The order static destruction can produce: a listener held by something
// constructed before the monitor - another singleton, a namespace-scope
// object - outlives the monitor's own static and deregisters after it is
// gone. The broadcaster nulls its listeners' back-pointers as it dies, so
// the listener's destructor is a no-op and nothing is reached after the fact.
auto tConnectivityListenerOutlivingTheMonitorIsHarmless =
    test("Connectivity/listenerOutlivingTheMonitorIsHarmless") = []
{
    auto calls = 0;
    auto listener = std::optional<EA::Listener> {};

    {
        auto monitor = Connectivity::Monitor {};

        listener.emplace(monitor, [&] { ++calls; });
        check(calls == 1);

        monitor.setState(stateWith(true, Connectivity::Interface::Wired));
        check(calls == 2);
    }

    listener.reset();
    check(calls == 2);
};

// The probe, against a server of the test's own.
namespace
{
using eacp::HTTP::Request;
using eacp::HTTP::Response;
using eacp::HTTP::Server;
using eacp::HTTP::ServerOptions;
using eacp::HTTP::ServerThreadingMode;
using eacp::Threads::runEventLoopUntil;
using eacp::Time::MS;

constexpr auto probeTimeout = MS {5000};
constexpr auto probeBody = "eacp-probe-ok";

Response portalPage()
{
    auto response = Response();
    response.statusCode = 200;
    response.content = "<html>log in</html>";
    return response;
}

Response probeAnswer()
{
    auto response = Response();
    response.statusCode = 200;
    response.content = probeBody;
    return response;
}

struct ProbeServer
{
    ProbeServer()
    {
        auto handler = [this](const Request&)
        {
            ++hits;
            return body.load() ? probeAnswer() : portalPage();
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

// A server whose handler blocks until the test lets it go, which has to run
// off the event loop the test is pumping. Its first answer is a portal's and
// every later one the probe's, so the first answer's landing is visible as
// reachable going false: the assumed-true start would hide a good one.
struct StallingProbeServer
{
    StallingProbeServer()
        : server(options())
    {
        auto handler = [this](const Request&)
        {
            auto hit = ++hits;
            gate.wait();
            return hit == 1 ? portalPage() : probeAnswer();
        };

        listening = server.listen(0, handler);
    }

    ~StallingProbeServer()
    {
        gate.release();
        server.stop();
    }

    static ServerOptions options()
    {
        auto serverOptions = ServerOptions();
        serverOptions.threading = ServerThreadingMode::ThreadPool;
        serverOptions.threadPoolSize = 2;
        return serverOptions;
    }

    Connectivity::ProbeOptions probeOptions(MS timeout) const
    {
        auto probe = Connectivity::ProbeOptions {};

        probe.url =
            "http://127.0.0.1:" + std::to_string(server.boundPort()) + "/probe";
        probe.expectedContent = probeBody;
        probe.interval = MS {60000};
        probe.retryInterval = MS {60000};
        probe.timeout = timeout;

        return probe;
    }

    StallGate gate;
    Server server;
    std::atomic<int> hits {0};
    bool listening = false;
};

bool pumpUntil(const std::function<bool()>& ready)
{
    return runEventLoopUntil(ready, probeTimeout);
}

void pumpFor(MS duration)
{
    runEventLoopUntil([] { return false; }, duration);
}
} // namespace

auto tConnectivityReachableMirrorsOnlineWithoutAProbe =
    test("Connectivity/reachableMirrorsOnlineWithoutAProbe") = []
{
    auto monitor = Connectivity::Monitor {};

    monitor.setState(stateWith(true, Connectivity::Interface::Wired));
    check(monitor.getState().reachable);
    check(monitor.getState().hasInternet());

    monitor.setState(stateWith(false, Connectivity::Interface::None));
    check(!monitor.getState().reachable);
    check(!monitor.getState().hasInternet());
};

auto tConnectivityProbeAssumesReachableUntilAnswered =
    test("Connectivity/probeAssumesReachableUntilAnswered") = []
{
    auto monitor = Connectivity::Monitor {};
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

    auto monitor = Connectivity::Monitor {};
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

    auto monitor = Connectivity::Monitor {};
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
    auto server = ProbeServer {};
    check(server.listening);

    auto probe = server.options();
    server.server.stop();

    auto monitor = Connectivity::Monitor {};
    monitor.setState(stateWith(true, Connectivity::Interface::Wired));
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

    auto monitor = Connectivity::Monitor {};
    monitor.setState(stateWith(true, Connectivity::Interface::Wired));
    monitor.startProbe(server.options());

    check(pumpUntil([&] { return !monitor.getState().reachable; }));

    monitor.stopProbe();

    check(!monitor.isProbing());
    check(monitor.getState().reachable);

    auto hitsAtStop = server.hits.load();
    pumpFor(MS {200});
    check(server.hits.load() == hitsAtStop);
};

auto tConnectivityProbeWaitsWhileOffline =
    test("Connectivity/probeWaitsWhileOffline") = []
{
    auto server = ProbeServer {};
    check(server.listening);

    auto monitor = Connectivity::Monitor {};
    monitor.setState(stateWith(false, Connectivity::Interface::None));
    monitor.startProbe(server.options());

    pumpFor(MS {200});
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

    auto monitor = Connectivity::Monitor {};
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
// would be followed by a second one moments later. The spans are wide
// because a shared CI runner can stall a process for most of a second.
auto tConnectivityProbeNowRestartsTheInterval =
    test("Connectivity/probeNowRestartsTheInterval") = []
{
    auto server = ProbeServer {};
    check(server.listening);

    auto monitor = Connectivity::Monitor {};
    monitor.setState(stateWith(true, Connectivity::Interface::Wired));

    auto probe = server.options();
    probe.interval = MS {2400};
    monitor.startProbe(probe);

    check(pumpUntil([&] { return server.hits.load() == 1; }));

    pumpFor(MS {1200});
    monitor.probeNow();
    check(pumpUntil([&] { return server.hits.load() == 2; }));

    // Past where the original schedule would have fired, short of the new one.
    pumpFor(MS {1800});
    check(server.hits.load() == 2);

    check(pumpUntil([&] { return server.hits.load() == 3; }));
};

// A restart keeps the last verdict: changing the options on a link the
// probe already found dead must not show it alive until the next fetch.
auto tConnectivityRestartingTheProbeKeepsTheVerdict =
    test("Connectivity/restartingTheProbeKeepsTheVerdict") = []
{
    auto server = ProbeServer {};
    check(server.listening);
    server.body.store(false);

    auto monitor = Connectivity::Monitor {};
    monitor.setState(stateWith(true, Connectivity::Interface::Wired));
    monitor.startProbe(server.options());

    check(pumpUntil([&] { return !monitor.getState().reachable; }));

    auto slower = server.options();
    slower.interval = MS {60000};
    monitor.startProbe(slower);

    check(!monitor.getState().reachable);
};

// probeNow while a fetch is still out asks nothing more: that fetch is the
// answer, and it restarts the schedule when it lands.
auto tConnectivityProbeNowDoesNotDoubleUpAnInFlightFetch =
    test("Connectivity/probeNowDoesNotDoubleUpAnInFlightFetch") = []
{
    auto server = StallingProbeServer {};
    check(server.listening);

    auto monitor = Connectivity::Monitor {};
    monitor.setState(stateWith(true, Connectivity::Interface::Wired));
    monitor.startProbe(server.probeOptions(MS {5000}));

    check(pumpUntil([&] { return server.hits.load() == 1; }));

    monitor.probeNow();
    monitor.probeNow();
    pumpFor(MS {100});
    check(server.hits.load() == 1);

    server.gate.release();
    check(pumpUntil([&] { return !monitor.getState().reachable; }));

    monitor.probeNow();
    check(pumpUntil([&] { return server.hits.load() == 2; }));
    check(pumpUntil([&] { return monitor.getState().reachable; }));
};

// A fetch that runs out of time is a failed one - every backend reports it
// in the response's error rather than throwing - and so the verdict is
// unreachable, from a server that never answers inside the probe's timeout.
auto tConnectivityProbeTimeoutCountsAsUnreachable =
    test("Connectivity/probeTimeoutCountsAsUnreachable") = []
{
    auto server = StallingProbeServer {};
    check(server.listening);

    auto monitor = Connectivity::Monitor {};
    monitor.setState(stateWith(true, Connectivity::Interface::Wired));
    monitor.startProbe(server.probeOptions(MS {100}));

    check(pumpUntil([&] { return !monitor.getState().reachable; }));
};

// A monitor destroyed with a fetch out and a tick scheduled: both land
// afterwards, on the message thread, and find nothing to report to.
auto tConnectivityDestroyingTheMonitorMidFetchIsHarmless =
    test("Connectivity/destroyingTheMonitorMidFetchIsHarmless") = []
{
    auto server = StallingProbeServer {};
    check(server.listening);

    {
        auto monitor = Connectivity::Monitor {};
        monitor.setState(stateWith(true, Connectivity::Interface::Wired));

        auto probe = server.probeOptions(MS {5000});
        probe.interval = MS {50};
        monitor.startProbe(probe);

        check(pumpUntil([&] { return server.hits.load() == 1; }));
    }

    server.gate.release();
    pumpFor(MS {300});
    check(server.hits.load() == 1);
};

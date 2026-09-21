#pragma once

#include <eacp/Core/Utils/Broadcaster.h>
#include <eacp/Core/Utils/Time.h>

#include <memory>
#include <string>

namespace eacp::Network::Connectivity
{
// Which kind of link the machine is reaching the network through. Other
// covers a link the platform named but eacp has no bucket for (a VPN, say);
// None is what an offline machine reports, and also what a platform that
// cannot name the link reports while online.
enum class Interface
{
    None,
    Wifi,
    Cellular,
    Wired,
    Other
};

std::string toString(Interface interfaceKind);

struct State
{
    // The machine has a usable path to the network. Not a promise that any
    // particular host answers: nothing is probed, this is what the OS says
    // about its own routes.
    bool online = false;

    // Something on the internet answered. Every platform's own verdict is
    // route-based and only ever refreshed when a link changes, so a wired
    // machine whose router lost its uplink - or a VM whose host did - stays
    // online forever by that measure. The probe (Monitor::startProbe) is
    // what catches it: while one runs, this is whether its last fetch was
    // answered as expected, assumed true until the first fetch says
    // otherwise. While none runs, this is simply online.
    bool reachable = false;

    Interface interfaceKind = Interface::None;

    // The link bills by the byte (a tethered phone, a metered Windows
    // connection). False where the platform does not say.
    bool expensive = false;

    // The user has asked for the link to be spared (macOS Low Data Mode, a
    // Windows constrained-internet hint). False where the platform does not
    // say.
    bool constrained = false;

    bool hasInternet() const { return online && reachable; }

    friend bool operator==(const State&, const State&) = default;
};

// What the probe fetches, what it expects back, and how often it asks. The
// defaults are the platform's own captive-portal endpoint - the same one
// its network indicator already talks to - and its expected body, so a
// portal's login page answering with 200 still counts as unreachable.
struct ProbeOptions
{
    static std::string defaultUrl();
    static std::string defaultExpectedContent();

    std::string url = defaultUrl();

    // The body must contain this. Empty accepts any 2xx.
    std::string expectedContent = defaultExpectedContent();

    // Between fetches after one that succeeded, and after one that failed,
    // so an outage is confirmed gone soon after it ends.
    Time::MS interval {30000};
    Time::MS retryInterval {5000};

    Time::MS timeout {5000};
};

// The process's one view of the network, and the broadcaster that says when
// it changed. The platform monitor starts on the first get() and runs for
// the life of the process.
//
// Message thread only, like everything the broadcaster reaches: the platform
// callbacks arrive on OS-owned threads and hop here before they touch
// anything. A listener pulls the new state from getState() - the trigger
// carries no payload.
//
//     EA::Listener listener {Network::Connectivity::Monitor::get(),
//                           [this] { refresh(); }};
//
// The default mode, TriggerNow, calls refresh() once as the listener is
// built, so a subscriber starts from the state it joined rather than from
// nothing.
struct Monitor : EA::BroadcasterOwner
{
    static Monitor& get();

    Monitor();
    ~Monitor() override;

    Monitor(const Monitor&) = delete;
    Monitor& operator=(const Monitor&) = delete;

    const State& getState() const { return state; }

    // The platform layer's way in. Stores the state and triggers when it
    // differs from the last one - so the broadcaster fires on changes, not
    // on every report the OS makes. What it carries for reachable is
    // ignored: that field is the monitor's own, from the probe.
    void setState(const State& newState);

    // Starts fetching options.url on a background thread, once now and then
    // on the interval, for as long as the machine is online: an offline
    // machine has nothing to ask. Opt-in, because it is traffic every half
    // minute on whatever link the machine has, metered ones included.
    // Calling it again replaces the options and restarts the schedule.
    void startProbe(const ProbeOptions& options = {});

    // Stops asking; reachable goes back to mirroring online. A fetch under
    // way is cancelled and its thread joined before this returns, as the
    // destructor does too, so no thread of the probe's outlives its monitor.
    void stopProbe();

    bool isProbing() const { return probing; }

    // The next fetch right away instead of at the scheduled time: when a
    // window regains focus, when a request of the app's own just failed -
    // whenever the user is about to act on the answer. The schedule
    // restarts from that fetch. Nothing when no probe runs.
    void probeNow();

private:
    struct Worker;

    void publish();
    void scheduleProbe(Time::MS delay);
    void onProbeDue(int generation);
    void runProbe();
    void onProbeResult(int generation, bool succeeded);
    void cancelWorker();

    State reported;
    State state;

    ProbeOptions probeOptions;
    bool probing = false;
    bool probeInFlight = false;
    bool lastProbeSucceeded = true;
    int probeGeneration = 0;

    // The thread a fetch runs on, and the cancel flag that ends it early.
    std::unique_ptr<Worker> worker;

    // What a scheduled tick holds in place of `this`: callAfter cannot be
    // cancelled, and the monitor may be gone by the time it fires.
    std::shared_ptr<Monitor*> alive {std::make_shared<Monitor*>(this)};
};

// All three are Monitor::get() in shorthand, so all three start the monitor
// and all three want the message thread.
bool isThisMachineOnline();
bool hasInternet();
const State& getState();
} // namespace eacp::Network::Connectivity

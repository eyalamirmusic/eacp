#pragma once

#include <eacp/Core/Utils/Broadcaster.h>

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

    Interface interfaceKind = Interface::None;

    // The link bills by the byte (a tethered phone, a metered Windows
    // connection). False where the platform does not say.
    bool expensive = false;

    // The user has asked for the link to be spared (macOS Low Data Mode, a
    // Windows constrained-internet hint). False where the platform does not
    // say.
    bool constrained = false;

    friend bool operator==(const State&, const State&) = default;
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

    const State& getState() const { return state; }

    // The platform layer's way in. Stores the state and triggers when it
    // differs from the last one - so the broadcaster fires on changes, not
    // on every report the OS makes.
    void setState(const State& newState);

private:
    State state;
};

// Both are Monitor::get() in shorthand, so both start the monitor and both
// want the message thread.
bool isThisMachineOnline();
const State& getState();
} // namespace eacp::Network::Connectivity

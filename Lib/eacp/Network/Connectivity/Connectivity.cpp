#include "ConnectivityInternal.h"

#include <eacp/Core/Threads/EventLoop.h>
#include <eacp/Core/Threads/ThreadUtils.h>
#include <eacp/Core/Utils/Singleton.h>

#include <mutex>

namespace eacp::Network::Connectivity
{
namespace
{
// Not Monitor::get(): that one starts the monitor, and the start reports a
// seed state through here before it returns.
Monitor& instance()
{
    return Singleton::get<Monitor>();
}

void ensureMonitoring()
{
    static auto started = std::once_flag {};

    std::call_once(started,
                   []
                   {
                       instance();
                       detail::startPlatformMonitoring();
                   });
}
} // namespace

namespace detail
{
void reportState(const State& state)
{
    if (Threads::isMainThread())
    {
        instance().setState(state);
        return;
    }

    Threads::callAsync([state] { instance().setState(state); });
}
} // namespace detail

Monitor& Monitor::get()
{
    ensureMonitoring();
    return instance();
}

void Monitor::setState(const State& newState)
{
    if (state == newState)
        return;

    state = newState;
    trigger();
}

std::string toString(Interface interfaceKind)
{
    switch (interfaceKind)
    {
        case Interface::Wifi:
            return "Wi-Fi";
        case Interface::Cellular:
            return "Cellular";
        case Interface::Wired:
            return "Wired";
        case Interface::Other:
            return "Other";
        case Interface::None:
            break;
    }

    return "None";
}

const State& getState()
{
    return Monitor::get().getState();
}

bool isThisMachineOnline()
{
    return getState().online;
}
} // namespace eacp::Network::Connectivity

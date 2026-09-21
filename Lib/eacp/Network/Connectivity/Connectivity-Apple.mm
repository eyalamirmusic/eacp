#import <Network/Network.h>

#include "ConnectivityInternal.h"

#include <arpa/inet.h>
#include <dispatch/dispatch.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>

namespace eacp::Network::Connectivity::detail
{
namespace
{
bool isLinkLocal(const sockaddr* address)
{
    if (address->sa_family == AF_INET)
    {
        const auto* v4 = reinterpret_cast<const sockaddr_in*>(address);
        return (ntohl(v4->sin_addr.s_addr) >> 16) == 0xa9fe;
    }

    const auto* v6 = reinterpret_cast<const sockaddr_in6*>(address);
    return IN6_IS_ADDR_LINKLOCAL(&v6->sin6_addr) != 0;
}

// The seed, and the only synchronous answer available here: nw_path_monitor
// delivers its first path asynchronously, which is milliseconds after the
// first isThisMachineOnline() call would have answered. An interface that is
// up with a routable address is what that call goes on until the monitor
// corrects it - which it then does, change event and all.
bool hasRoutableInterface()
{
    ifaddrs* list = nullptr;

    if (getifaddrs(&list) != 0)
        return false;

    auto found = false;

    for (const auto* it = list; it != nullptr && !found; it = it->ifa_next)
    {
        if (it->ifa_addr == nullptr)
            continue;

        if (it->ifa_addr->sa_family != AF_INET
            && it->ifa_addr->sa_family != AF_INET6)
            continue;

        constexpr auto usable = IFF_UP | IFF_RUNNING;

        if ((it->ifa_flags & usable) != usable)
            continue;

        if ((it->ifa_flags & IFF_LOOPBACK) != 0)
            continue;

        found = !isLinkLocal(it->ifa_addr);
    }

    freeifaddrs(list);
    return found;
}

Interface interfaceOf(nw_path_t path)
{
    if (nw_path_uses_interface_type(path, nw_interface_type_wifi))
        return Interface::Wifi;

    if (nw_path_uses_interface_type(path, nw_interface_type_cellular))
        return Interface::Cellular;

    if (nw_path_uses_interface_type(path, nw_interface_type_wired))
        return Interface::Wired;

    return Interface::Other;
}

State stateOf(nw_path_t path)
{
    auto state = State {};

    state.online = nw_path_get_status(path) == nw_path_status_satisfied;
    state.interfaceKind =
        state.online ? interfaceOf(path) : Interface::None;
    state.expensive = nw_path_is_expensive(path);
    state.constrained = nw_path_is_constrained(path);

    return state;
}
} // namespace

void startPlatformMonitoring()
{
    auto seed = State {};
    seed.online = hasRoutableInterface();
    seed.interfaceKind = seed.online ? Interface::Other : Interface::None;
    reportState(seed);

    // Both live for the process: the monitor is never cancelled, so nothing
    // here is released.
    static auto monitor = nw_path_monitor_create();
    static auto queue = dispatch_queue_create("com.eacp.network.availability",
                                              DISPATCH_QUEUE_SERIAL);

    nw_path_monitor_set_queue(monitor, queue);
    nw_path_monitor_set_update_handler(monitor,
                                       ^(nw_path_t path)
                                       {
                                           reportState(stateOf(path));
                                       });
    nw_path_monitor_start(monitor);
}
} // namespace eacp::Network::Connectivity::detail

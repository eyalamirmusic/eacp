#include <winsock2.h>
#include <ws2tcpip.h>

#include <iphlpapi.h>
#include <netioapi.h>

#include <eacp/Core/Utils/WinInclude.h>

#include "ConnectivityInternal.h"

#include <vector>

namespace eacp::Network::Connectivity::detail
{
namespace
{
Interface interfaceOf(IFTYPE type)
{
    switch (type)
    {
        case IF_TYPE_IEEE80211:
            return Interface::Wifi;
        case IF_TYPE_ETHERNET_CSMACD:
        case IF_TYPE_ISO88025_TOKENRING:
            return Interface::Wired;
        case IF_TYPE_WWANPP:
        case IF_TYPE_WWANPP2:
            return Interface::Cellular;
        default:
            return Interface::Other;
    }
}

struct AdapterView
{
    bool online = false;
    Interface kind = Interface::None;
};

// The connectivity hint says how good the connection is but never what it
// runs over, so the interface kind comes from the adapter list: the first
// adapter that is up, is not loopback, and has a gateway to send through.
AdapterView readAdapters()
{
    constexpr auto flags = GAA_FLAG_INCLUDE_GATEWAYS | GAA_FLAG_SKIP_ANYCAST
                           | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER;

    auto size = ULONG {16 * 1024};
    auto buffer = std::vector<unsigned char>(size);
    auto result = DWORD {ERROR_BUFFER_OVERFLOW};

    for (auto attempt = 0; attempt < 4 && result == ERROR_BUFFER_OVERFLOW; ++attempt)
    {
        buffer.resize(size);

        result = GetAdaptersAddresses(
            AF_UNSPEC,
            flags,
            nullptr,
            reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data()),
            &size);
    }

    auto view = AdapterView {};

    if (result != NO_ERROR)
        return view;

    for (auto* adapter = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data());
         adapter != nullptr;
         adapter = adapter->Next)
    {
        if (adapter->OperStatus != IfOperStatusUp)
            continue;

        if (adapter->IfType == IF_TYPE_SOFTWARE_LOOPBACK)
            continue;

        if (adapter->FirstGatewayAddress == nullptr
            || adapter->FirstUnicastAddress == nullptr)
            continue;

        view.online = true;
        view.kind = interfaceOf(adapter->IfType);
        break;
    }

    return view;
}

State stateFromAdaptersOnly()
{
    auto adapters = readAdapters();
    auto state = State {};

    state.online = adapters.online;
    state.interfaceKind = adapters.kind;

    return state;
}

State stateFrom(const NL_NETWORK_CONNECTIVITY_HINT& hint)
{
    auto level = hint.ConnectivityLevel;
    auto cost = hint.ConnectivityCost;
    auto state = State {};

    state.online = level == NetworkConnectivityLevelHintInternetAccess
                   || level == NetworkConnectivityLevelHintConstrainedInternetAccess;

    state.constrained =
        level == NetworkConnectivityLevelHintConstrainedInternetAccess;

    state.expensive = cost == NetworkConnectivityCostHintFixed
                      || cost == NetworkConnectivityCostHintVariable
                      || hint.OverDataLimit != FALSE || hint.Roaming != FALSE;

    if (state.online)
    {
        auto adapters = readAdapters();
        state.interfaceKind =
            adapters.kind == Interface::None ? Interface::Other : adapters.kind;
    }

    return state;
}

void NETIOAPI_API_ onHintChanged(void*, NL_NETWORK_CONNECTIVITY_HINT hint)
{
    reportState(stateFrom(hint));
}

using GetHintFn = DWORD(NETIOAPI_API_*)(NL_NETWORK_CONNECTIVITY_HINT*);

using NotifyHintChangeFn = DWORD(NETIOAPI_API_*)(
    PNETWORK_CONNECTIVITY_HINT_CHANGE_CALLBACK, PVOID, BOOLEAN, PHANDLE);
} // namespace

void startPlatformMonitoring()
{
    // Resolved rather than imported: both arrived in Windows 10 2004, and an
    // import of a missing symbol stops the process from starting at all.
    // Without them the adapter list is the whole answer, once, with no
    // change events - which is still better than refusing to launch.
    auto* iphlpapi = GetModuleHandleW(L"iphlpapi.dll");

    auto getHint =
        iphlpapi != nullptr
            ? procAddress<GetHintFn>(iphlpapi, "GetNetworkConnectivityHint")
            : nullptr;

    auto notifyHintChange =
        iphlpapi != nullptr ? procAddress<NotifyHintChangeFn>(
                                  iphlpapi, "NotifyNetworkConnectivityHintChange")
                            : nullptr;

    if (getHint != nullptr)
    {
        auto hint = NL_NETWORK_CONNECTIVITY_HINT {};

        if (getHint(&hint) == NO_ERROR)
            reportState(stateFrom(hint));
    }
    else
    {
        reportState(stateFromAdaptersOnly());
    }

    if (notifyHintChange == nullptr)
        return;

    // Never cancelled, so the handle is never closed: the monitor lives as
    // long as the process does.
    static auto notification = HANDLE {};

    notifyHintChange(onHintChanged, nullptr, TRUE, &notification);
}
} // namespace eacp::Network::Connectivity::detail

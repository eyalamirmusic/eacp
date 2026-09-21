#include "ConnectivityInternal.h"

#include <asm/types.h>
#include <linux/netlink.h>
#include <linux/rtnetlink.h>
#include <net/if.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

namespace eacp::Network::Connectivity::detail
{
namespace
{
constexpr auto receiveBufferSize = std::size_t {8192};

struct Socket
{
    explicit Socket(int groups)
    {
        fd = ::socket(AF_NETLINK, SOCK_RAW | SOCK_CLOEXEC, NETLINK_ROUTE);

        if (fd < 0)
            return;

        auto address = sockaddr_nl {};
        address.nl_family = AF_NETLINK;
        address.nl_groups = static_cast<unsigned int>(groups);

        if (::bind(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) < 0)
        {
            ::close(fd);
            fd = -1;
        }
    }

    ~Socket()
    {
        if (fd >= 0)
            ::close(fd);
    }

    Socket(const Socket&) = delete;
    Socket& operator=(const Socket&) = delete;

    bool isOpen() const { return fd >= 0; }

    int fd = -1;
};

bool isWireless(const std::string& name)
{
    return ::access(("/sys/class/net/" + name + "/wireless").c_str(), F_OK) == 0;
}

bool startsWith(const std::string& name, const char* prefix)
{
    return name.rfind(prefix, 0) == 0;
}

Interface interfaceOfIndex(int index)
{
    char nameBuffer[IF_NAMESIZE] = {};

    if (::if_indextoname(static_cast<unsigned int>(index), nameBuffer) == nullptr)
        return Interface::Other;

    auto name = std::string {nameBuffer};

    if (isWireless(name) || startsWith(name, "wl"))
        return Interface::Wifi;

    if (startsWith(name, "wwan") || startsWith(name, "ppp")
        || startsWith(name, "rmnet"))
        return Interface::Cellular;

    if (startsWith(name, "en") || startsWith(name, "eth"))
        return Interface::Wired;

    return Interface::Other;
}

// A default route (dst_len == 0) in the main table is the whole definition
// of online here: the kernel has somewhere to send a packet it has no more
// specific route for.
bool isDefaultRoute(const rtmsg& route)
{
    return route.rtm_dst_len == 0 && route.rtm_table == RT_TABLE_MAIN
           && route.rtm_type == RTN_UNICAST
           && (route.rtm_family == AF_INET || route.rtm_family == AF_INET6);
}

int outgoingInterfaceOf(const nlmsghdr* header, const rtmsg* route)
{
    auto remaining = static_cast<unsigned int>(RTM_PAYLOAD(header));

    for (auto* attribute = RTM_RTA(const_cast<rtmsg*>(route));
         RTA_OK(attribute, remaining);
         attribute = RTA_NEXT(attribute, remaining))
    {
        if (attribute->rta_type == RTA_OIF)
            return *static_cast<const int*>(RTA_DATA(attribute));
    }

    return 0;
}

bool sendRouteDumpRequest(int fd)
{
    struct Request
    {
        nlmsghdr header;
        rtmsg route;
    };

    auto request = Request {};

    request.header.nlmsg_len = NLMSG_LENGTH(sizeof(rtmsg));
    request.header.nlmsg_type = RTM_GETROUTE;
    request.header.nlmsg_flags = NLM_F_REQUEST | NLM_F_DUMP;
    request.header.nlmsg_seq = 1;
    request.route.rtm_family = AF_UNSPEC;
    request.route.rtm_table = RT_TABLE_MAIN;

    return ::send(fd, &request, request.header.nlmsg_len, 0) >= 0;
}

// The authoritative answer, and cheap enough to be the answer every time:
// one dump, read to its NLMSG_DONE. The notification socket only says that
// something moved - what it moved to is read back here rather than tracked
// as a delta, which is a state machine that can drift out of step with the
// kernel and never come back.
State queryDefaultRoute()
{
    auto state = State {};
    auto socket = Socket {0};

    if (!socket.isOpen() || !sendRouteDumpRequest(socket.fd))
        return state;

    auto timeout = timeval {};
    timeout.tv_sec = 2;
    ::setsockopt(socket.fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));

    auto buffer = std::vector<char>(receiveBufferSize);

    for (auto done = false; !done;)
    {
        auto received = ::recv(socket.fd, buffer.data(), buffer.size(), 0);

        if (received <= 0)
        {
            if (received < 0 && errno == EINTR)
                continue;

            break;
        }

        auto remaining = static_cast<unsigned int>(received);

        for (auto* header = reinterpret_cast<nlmsghdr*>(buffer.data());
             NLMSG_OK(header, remaining) && !done;
             header = NLMSG_NEXT(header, remaining))
        {
            if (header->nlmsg_type == NLMSG_DONE
                || header->nlmsg_type == NLMSG_ERROR)
            {
                done = true;
                break;
            }

            if (header->nlmsg_type != RTM_NEWROUTE)
                continue;

            const auto* route = static_cast<const rtmsg*>(NLMSG_DATA(header));

            if (!isDefaultRoute(*route))
                continue;

            auto interfaceIndex = outgoingInterfaceOf(header, route);

            state.online = true;
            state.interfaceKind = interfaceIndex != 0
                                      ? interfaceOfIndex(interfaceIndex)
                                      : Interface::Other;
            done = true;
        }
    }

    return state;
}

bool isInterestingMessage(const nlmsghdr* header)
{
    switch (header->nlmsg_type)
    {
        case RTM_NEWROUTE:
        case RTM_DELROUTE:
            return static_cast<const rtmsg*>(NLMSG_DATA(header))->rtm_dst_len == 0;

        case RTM_NEWLINK:
        case RTM_DELLINK:
        case RTM_NEWADDR:
        case RTM_DELADDR:
            return true;

        default:
            return false;
    }
}

bool drainAndTestForChange(int fd, std::vector<char>& buffer)
{
    auto interesting = false;

    while (true)
    {
        auto received = ::recv(fd, buffer.data(), buffer.size(), MSG_DONTWAIT);

        if (received <= 0)
        {
            if (received < 0 && errno == EINTR)
                continue;

            // ENOBUFS is the kernel saying it dropped notifications because
            // this socket fell behind. What was dropped is unknowable, so
            // the only safe reading of it is that something changed.
            if (received < 0 && errno != EAGAIN && errno != EWOULDBLOCK)
                interesting = true;

            break;
        }

        auto remaining = static_cast<unsigned int>(received);

        for (auto* header = reinterpret_cast<nlmsghdr*>(buffer.data());
             NLMSG_OK(header, remaining);
             header = NLMSG_NEXT(header, remaining))
        {
            if (isInterestingMessage(header))
                interesting = true;
        }
    }

    return interesting;
}

// Its own thread rather than Threads::addLoopSource: the monitor starts on
// whatever thread first asks whether the machine is online, and a console
// app that never runs an event loop still has to get the answer right.
void runMonitorThread()
{
    auto socket = Socket {RTMGRP_LINK | RTMGRP_IPV4_ROUTE | RTMGRP_IPV6_ROUTE
                          | RTMGRP_IPV4_IFADDR | RTMGRP_IPV6_IFADDR};

    if (!socket.isOpen())
        return;

    auto buffer = std::vector<char>(receiveBufferSize);

    while (true)
    {
        auto entry = pollfd {socket.fd, POLLIN, 0};

        if (::poll(&entry, 1, -1) < 0)
        {
            if (errno == EINTR)
                continue;

            return;
        }

        if ((entry.revents & POLLIN) == 0)
            return;

        if (drainAndTestForChange(socket.fd, buffer))
            reportState(queryDefaultRoute());
    }
}
} // namespace

void startPlatformMonitoring()
{
    reportState(queryDefaultRoute());

    // Detached and never joined: the monitor lives as long as the process.
    std::thread {runMonitorThread}.detach();
}
} // namespace eacp::Network::Connectivity::detail

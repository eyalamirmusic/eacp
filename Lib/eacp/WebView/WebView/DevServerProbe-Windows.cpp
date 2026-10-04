#include "../Common.h"
#include "DevServerProbeInternal.h"

#include <winsock2.h>
#include <ws2tcpip.h>

namespace eacp::Graphics
{
namespace
{
class WinsockGuard
{
public:
    WinsockGuard()
    {
        auto data = WSADATA {};
        ok = WSAStartup(MAKEWORD(2, 2), &data) == 0;
    }

    ~WinsockGuard()
    {
        if (ok)
            WSACleanup();
    }

    WinsockGuard(const WinsockGuard&) = delete;
    WinsockGuard& operator=(const WinsockGuard&) = delete;

    bool ready() const { return ok; }

private:
    bool ok = false;
};

class SocketGuard
{
public:
    explicit SocketGuard(SOCKET s)
        : sock(s)
    {
    }

    ~SocketGuard()
    {
        if (sock != INVALID_SOCKET)
            closesocket(sock);
    }

    SocketGuard(const SocketGuard&) = delete;
    SocketGuard& operator=(const SocketGuard&) = delete;

    SOCKET get() const { return sock; }

private:
    SOCKET sock;
};

void setNonBlocking(SOCKET s)
{
    auto mode = u_long {1};
    ioctlsocket(s, FIONBIO, &mode);
}

bool waitWritable(SOCKET s, int timeoutMs)
{
    auto writefds = fd_set {};
    FD_ZERO(&writefds);
    FD_SET(s, &writefds);

    auto tv = timeval {};
    tv.tv_sec = timeoutMs / 1000;
    tv.tv_usec = (timeoutMs % 1000) * 1000;

    return ::select(0, nullptr, &writefds, nullptr, &tv) > 0;
}

bool socketHasNoError(SOCKET s)
{
    auto err = 0;
    auto len = (int) sizeof(err);
    auto rc = ::getsockopt(s, SOL_SOCKET, SO_ERROR, (char*) &err, &len);
    return rc == 0 && err == 0;
}

bool tryConnect(const addrinfo& ai, int timeoutMs)
{
    auto sock = SocketGuard(::socket(ai.ai_family, ai.ai_socktype, ai.ai_protocol));

    if (sock.get() == INVALID_SOCKET)
        return false;

    setNonBlocking(sock.get());

    auto rc = ::connect(sock.get(), ai.ai_addr, (int) ai.ai_addrlen);

    if (rc == 0)
        return true;

    if (WSAGetLastError() != WSAEWOULDBLOCK)
        return false;

    if (!waitWritable(sock.get(), timeoutMs))
        return false;

    return socketHasNoError(sock.get());
}
} // namespace

bool probeTCP(const std::string& host, int port, int timeoutMs)
{
    auto winsock = WinsockGuard();

    if (!winsock.ready())
        return false;

    auto hints = addrinfo {};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    addrinfo* res = nullptr;

    if (getaddrinfo(host.c_str(), std::to_string(port).c_str(), &hints, &res) != 0)
        return false;

    auto connected = false;

    for (auto ai = res; ai != nullptr; ai = ai->ai_next)
    {
        if (tryConnect(*ai, timeoutMs))
        {
            connected = true;
            break;
        }
    }

    freeaddrinfo(res);
    return connected;
}
} // namespace eacp::Graphics

#include "../Common.h"
#include "DevServerProbeInternal.h"

#include <cerrno>
#include <fcntl.h>
#include <netdb.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>

namespace eacp::Graphics
{
namespace
{
constexpr auto kInvalidSocket = -1;

class SocketGuard
{
public:
    explicit SocketGuard(int s)
        : sock(s)
    {
    }

    ~SocketGuard()
    {
        if (sock != kInvalidSocket)
            ::close(sock);
    }

    SocketGuard(const SocketGuard&) = delete;
    SocketGuard& operator=(const SocketGuard&) = delete;

    int get() const { return sock; }

private:
    int sock;
};

void setNonBlocking(int s)
{
    auto flags = fcntl(s, F_GETFL, 0);
    fcntl(s, F_SETFL, flags | O_NONBLOCK);
}

bool waitWritable(int s, int timeoutMs)
{
    auto writefds = fd_set {};
    FD_ZERO(&writefds);
    FD_SET(s, &writefds);

    auto tv = timeval {};
    tv.tv_sec = timeoutMs / 1000;
    tv.tv_usec = (timeoutMs % 1000) * 1000;

    return ::select(s + 1, nullptr, &writefds, nullptr, &tv) > 0;
}

bool socketHasNoError(int s)
{
    auto err = 0;
    auto len = (socklen_t) sizeof(err);
    auto rc = ::getsockopt(s, SOL_SOCKET, SO_ERROR, &err, &len);
    return rc == 0 && err == 0;
}

bool tryConnect(const addrinfo& ai, int timeoutMs)
{
    auto sock = SocketGuard(::socket(ai.ai_family, ai.ai_socktype, ai.ai_protocol));

    if (sock.get() == kInvalidSocket)
        return false;

    setNonBlocking(sock.get());

    auto rc = ::connect(sock.get(), ai.ai_addr, ai.ai_addrlen);

    if (rc == 0)
        return true;

    if (errno != EINPROGRESS)
        return false;

    if (!waitWritable(sock.get(), timeoutMs))
        return false;

    return socketHasNoError(sock.get());
}
} // namespace

bool probeTCP(const std::string& host, int port, int timeoutMs)
{
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

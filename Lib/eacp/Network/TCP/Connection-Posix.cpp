#include "ConnectionInternal.h"

#include <arpa/inet.h>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <string>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

namespace eacp::TCP::detail
{
namespace
{
constexpr auto kInvalidFd = -1;

[[noreturn]] void throwErrno(const std::string& context)
{
    throw Error(context + ": " + std::strerror(errno));
}

void setNonBlocking(int fd, bool enabled)
{
    auto flags = ::fcntl(fd, F_GETFL, 0);
    ::fcntl(fd, F_SETFL, enabled ? (flags | O_NONBLOCK) : (flags & ~O_NONBLOCK));
}

timeval toTimeval(std::chrono::milliseconds timeout)
{
    auto tv = timeval {};
    tv.tv_sec = (time_t) (timeout.count() / 1000);
    tv.tv_usec = (suseconds_t) ((timeout.count() % 1000) * 1000);
    return tv;
}

bool waitWritable(int fd, std::chrono::milliseconds timeout)
{
    auto writable = fd_set {};
    FD_ZERO(&writable);
    FD_SET(fd, &writable);

    auto tv = toTimeval(timeout);
    return ::select(fd + 1, nullptr, &writable, nullptr, &tv) > 0;
}

int pendingSocketError(int fd)
{
    auto error = 0;
    auto length = (socklen_t) sizeof(error);
    if (::getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &length) < 0)
        return errno;
    return error;
}

void armTimeouts(int fd, std::chrono::milliseconds ioTimeout)
{
    auto tv = toTimeval(ioTimeout);
    ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    ::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

#ifdef SO_NOSIGPIPE
    auto on = 1;
    ::setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &on, sizeof(on));
#endif
}

// Connects a single resolved address. Returns a ready fd, or -1 with why
// filled in so the caller can report the last failure across candidates.
int tryConnect(const addrinfo& candidate,
               std::chrono::milliseconds connectTimeout,
               std::chrono::milliseconds ioTimeout,
               std::string& why)
{
    auto fd =
        ::socket(candidate.ai_family, candidate.ai_socktype, candidate.ai_protocol);
    if (fd < 0)
    {
        why = std::strerror(errno);
        return kInvalidFd;
    }

    setNonBlocking(fd, true);
    auto result = ::connect(fd, candidate.ai_addr, candidate.ai_addrlen);

    if (result != 0)
    {
        if (errno != EINPROGRESS)
        {
            why = std::strerror(errno);
            ::close(fd);
            return kInvalidFd;
        }

        if (! waitWritable(fd, connectTimeout))
        {
            why = "connect timed out";
            ::close(fd);
            return kInvalidFd;
        }

        if (auto error = pendingSocketError(fd); error != 0)
        {
            why = std::strerror(error);
            ::close(fd);
            return kInvalidFd;
        }
    }

    setNonBlocking(fd, false);
    armTimeouts(fd, ioTimeout);
    return fd;
}

bool timedOut() { return errno == EAGAIN || errno == EWOULDBLOCK; }

int sendFlags()
{
#ifdef MSG_NOSIGNAL
    return MSG_NOSIGNAL;
#else
    return 0;
#endif
}
} // namespace

NativeSocket socketConnect(const Address& address,
                           std::chrono::milliseconds connectTimeout,
                           std::chrono::milliseconds ioTimeout)
{
    auto hints = addrinfo {};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    auto port = std::to_string(address.port);
    addrinfo* resolved = nullptr;
    auto rc = ::getaddrinfo(address.host.c_str(), port.c_str(), &hints, &resolved);
    if (rc != 0)
        throw Error("Couldn't resolve " + address.host + ": " + ::gai_strerror(rc));

    auto why = std::string {"no addresses resolved"};
    for (auto candidate = resolved; candidate != nullptr;
         candidate = candidate->ai_next)
    {
        auto fd = tryConnect(*candidate, connectTimeout, ioTimeout, why);
        if (fd != kInvalidFd)
        {
            ::freeaddrinfo(resolved);
            return (NativeSocket) fd;
        }
    }

    ::freeaddrinfo(resolved);
    throw Error("Couldn't connect to " + address.host + ":" + port + ": " + why);
}

void socketClose(NativeSocket socket) noexcept
{
    if (socket != invalidSocket)
        ::close((int) socket);
}

std::size_t socketSend(NativeSocket socket, const char* data, std::size_t length)
{
    auto sent = ::send((int) socket, data, length, sendFlags());
    if (sent < 0)
        throwErrno(timedOut() ? "send timed out" : "send");
    return (std::size_t) sent;
}

std::size_t socketReceive(NativeSocket socket, char* buffer, std::size_t length)
{
    auto received = ::recv((int) socket, buffer, length, 0);
    if (received < 0)
        throwErrno(timedOut() ? "receive timed out" : "receive");
    return (std::size_t) received;
}

NativeSocket socketListen(std::uint16_t port, std::uint16_t& boundPort)
{
    auto fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0)
        throwErrno("socket");

    auto yes = 1;
    ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

    auto addr = sockaddr_in {};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(port);

    if (::bind(fd, (sockaddr*) &addr, sizeof(addr)) < 0)
    {
        auto saved = errno;
        ::close(fd);
        errno = saved;
        throwErrno("bind");
    }

    if (::listen(fd, SOMAXCONN) < 0)
    {
        auto saved = errno;
        ::close(fd);
        errno = saved;
        throwErrno("listen");
    }

    auto bound = sockaddr_in {};
    auto length = (socklen_t) sizeof(bound);
    if (::getsockname(fd, (sockaddr*) &bound, &length) == 0)
        boundPort = ntohs(bound.sin_port);

    return (NativeSocket) fd;
}

NativeSocket socketAccept(NativeSocket listenSocket,
                          std::chrono::milliseconds acceptTimeout,
                          std::chrono::milliseconds ioTimeout,
                          Address& peer)
{
    auto lfd = (int) listenSocket;

    auto readable = fd_set {};
    FD_ZERO(&readable);
    FD_SET(lfd, &readable);

    auto tv = toTimeval(acceptTimeout);
    auto ready = ::select(lfd + 1, &readable, nullptr, nullptr, &tv);
    if (ready == 0)
        throw Error("accept timed out");
    if (ready < 0)
        throwErrno("accept");

    auto addr = sockaddr_in {};
    auto length = (socklen_t) sizeof(addr);
    auto fd = ::accept(lfd, (sockaddr*) &addr, &length);
    if (fd < 0)
        throwErrno("accept");

    char host[INET_ADDRSTRLEN] = {};
    if (::inet_ntop(AF_INET, &addr.sin_addr, host, sizeof(host)) != nullptr)
        peer.host = host;
    peer.port = ntohs(addr.sin_port);

    armTimeouts(fd, ioTimeout);
    return (NativeSocket) fd;
}

} // namespace eacp::TCP::detail

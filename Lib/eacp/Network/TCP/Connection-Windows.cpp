#include "ConnectionInternal.h"

#include <winsock2.h>
#include <ws2tcpip.h>

namespace eacp::TCP::detail
{
namespace
{
// Winsock must stay initialised for the whole life of any socket, so unlike
// the per-call guards elsewhere this is a process-lifetime singleton.
void ensureWinsockInitialized()
{
    static auto started = []
    {
        auto data = WSADATA {};
        return WSAStartup(MAKEWORD(2, 2), &data) == 0;
    }();
    (void) started;
}

[[noreturn]] void throwLastError(const std::string& context)
{
    throw Error(context + ": Winsock error " + std::to_string(WSAGetLastError()));
}

void setNonBlocking(SOCKET socket, bool enabled)
{
    auto mode = (u_long) (enabled ? 1 : 0);
    ::ioctlsocket(socket, FIONBIO, &mode);
}

bool waitWritable(SOCKET socket, Time::MS timeout)
{
    auto writable = fd_set {};
    FD_ZERO(&writable);
    FD_SET(socket, &writable);

    // Winsock signals a FAILED non-blocking connect (e.g. connection refused by a
    // closed port) ONLY in the exception set — never as writable, unlike POSIX.
    // Without watching it, select() ignores the refusal and blocks for the entire
    // connectTimeout (15 s for the Ableton probe), stalling whatever thread the
    // connect runs on. The caller distinguishes success from failure right after
    // via pendingSocketError(), so reporting "ready" on either event is correct.
    auto failed = fd_set {};
    FD_ZERO(&failed);
    FD_SET(socket, &failed);

    auto tv = timeval {};
    tv.tv_sec = (long) (timeout.count / 1000);
    tv.tv_usec = (long) ((timeout.count % 1000) * 1000);

    auto* deadline = timeout.count > 0 ? &tv : nullptr; // null = block forever
    return ::select(0, nullptr, &writable, &failed, deadline) > 0;
}

int pendingSocketError(SOCKET socket)
{
    auto error = 0;
    auto length = (int) sizeof(error);
    if (::getsockopt(socket, SOL_SOCKET, SO_ERROR, (char*) &error, &length) < 0)
        return WSAGetLastError();
    return error;
}

void armTimeouts(SOCKET socket, Time::MS ioTimeout)
{
    if (ioTimeout.count <= 0) // otherwise leave the socket blocking forever
        return;

    auto millis = (DWORD) ioTimeout.count;
    ::setsockopt(
        socket, SOL_SOCKET, SO_RCVTIMEO, (const char*) &millis, sizeof(millis));
    ::setsockopt(
        socket, SOL_SOCKET, SO_SNDTIMEO, (const char*) &millis, sizeof(millis));
}

// Connects a single resolved address. Returns a ready socket, or
// INVALID_SOCKET with why filled in so the caller can report the last
// failure across candidates.
SOCKET tryConnect(const addrinfo& candidate,
                  Time::MS connectTimeout,
                  Time::MS ioTimeout,
                  std::string& why)
{
    auto socket =
        ::socket(candidate.ai_family, candidate.ai_socktype, candidate.ai_protocol);
    if (socket == INVALID_SOCKET)
    {
        why = "socket() failed";
        return INVALID_SOCKET;
    }

    setNonBlocking(socket, true);
    auto result = ::connect(socket, candidate.ai_addr, (int) candidate.ai_addrlen);

    if (result != 0)
    {
        if (WSAGetLastError() != WSAEWOULDBLOCK)
        {
            why = "connect failed";
            ::closesocket(socket);
            return INVALID_SOCKET;
        }

        if (!waitWritable(socket, connectTimeout))
        {
            why = "connect timed out";
            ::closesocket(socket);
            return INVALID_SOCKET;
        }

        if (pendingSocketError(socket) != 0)
        {
            why = "connect failed";
            ::closesocket(socket);
            return INVALID_SOCKET;
        }
    }

    setNonBlocking(socket, false);
    armTimeouts(socket, ioTimeout);
    return socket;
}

bool timedOut()
{
    return WSAGetLastError() == WSAETIMEDOUT;
}
} // namespace

NativeSocket socketConnect(const Address& address,
                           Time::MS connectTimeout,
                           Time::MS ioTimeout)
{
    ensureWinsockInitialized();

    auto hints = addrinfo {};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    auto port = std::to_string(address.port);
    addrinfo* resolved = nullptr;
    if (::getaddrinfo(address.host.c_str(), port.c_str(), &hints, &resolved) != 0)
        throw Error("Couldn't resolve " + address.host + ": Winsock error "
                    + std::to_string(WSAGetLastError()));

    auto why = std::string {"no addresses resolved"};
    for (auto candidate = resolved; candidate != nullptr;
         candidate = candidate->ai_next)
    {
        auto socket = tryConnect(*candidate, connectTimeout, ioTimeout, why);
        if (socket != INVALID_SOCKET)
        {
            ::freeaddrinfo(resolved);
            return (NativeSocket) socket;
        }
    }

    ::freeaddrinfo(resolved);
    throw Error("Couldn't connect to " + address.host + ":" + port + ": " + why);
}

void socketClose(NativeSocket socket) noexcept
{
    if (socket != invalidSocket)
        ::closesocket((SOCKET) socket);
}

std::size_t socketSend(NativeSocket socket, const char* data, std::size_t length)
{
    auto sent = ::send((SOCKET) socket, data, (int) length, 0);
    if (sent == SOCKET_ERROR)
    {
        if (timedOut())
            throw TimeoutError("send timed out");
        throwLastError("send");
    }
    return (std::size_t) sent;
}

std::size_t socketReceive(NativeSocket socket, char* buffer, std::size_t length)
{
    auto received = ::recv((SOCKET) socket, buffer, (int) length, 0);
    if (received == SOCKET_ERROR)
    {
        if (timedOut())
            throw TimeoutError("receive timed out");
        throwLastError("receive");
    }
    return (std::size_t) received;
}

NativeSocket socketListen(std::uint16_t port, std::uint16_t& boundPort)
{
    ensureWinsockInitialized();

    auto sock = ::socket(AF_INET, SOCK_STREAM, 0);
    if (sock == INVALID_SOCKET)
        throwLastError("socket");

    auto yes = 1;
    ::setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, (const char*) &yes, sizeof(yes));

    auto addr = sockaddr_in {};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(port);

    if (::bind(sock, (sockaddr*) &addr, sizeof(addr)) == SOCKET_ERROR)
    {
        ::closesocket(sock);
        throwLastError("bind");
    }

    if (::listen(sock, SOMAXCONN) == SOCKET_ERROR)
    {
        ::closesocket(sock);
        throwLastError("listen");
    }

    auto bound = sockaddr_in {};
    auto length = (int) sizeof(bound);
    if (::getsockname(sock, (sockaddr*) &bound, &length) == 0)
        boundPort = ntohs(bound.sin_port);

    return (NativeSocket) sock;
}

NativeSocket socketAccept(NativeSocket listenSocket,
                          Time::MS acceptTimeout,
                          Time::MS ioTimeout,
                          Address& peer)
{
    auto lsock = (SOCKET) listenSocket;

    auto readable = fd_set {};
    FD_ZERO(&readable);
    FD_SET(lsock, &readable);

    auto tv = timeval {};
    tv.tv_sec = (long) (acceptTimeout.count / 1000);
    tv.tv_usec = (long) ((acceptTimeout.count % 1000) * 1000);

    auto* deadline = acceptTimeout.count > 0 ? &tv : nullptr; // null = forever
    auto ready = ::select(0, &readable, nullptr, nullptr, deadline);
    if (ready == 0)
        throw TimeoutError("accept timed out");
    if (ready == SOCKET_ERROR)
        throwLastError("accept");

    auto addr = sockaddr_in {};
    auto length = (int) sizeof(addr);
    auto sock = ::accept(lsock, (sockaddr*) &addr, &length);
    if (sock == INVALID_SOCKET)
        throwLastError("accept");

    char host[INET_ADDRSTRLEN] = {};
    if (::inet_ntop(AF_INET, &addr.sin_addr, host, sizeof(host)) != nullptr)
        peer.host = host;
    peer.port = ntohs(addr.sin_port);

    armTimeouts(sock, ioTimeout);
    return (NativeSocket) sock;
}

} // namespace eacp::TCP::detail

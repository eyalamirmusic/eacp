#pragma once

#include "Connection.h"

// The platform socket backend, implemented in Connection-Posix.cpp and
// Connection-Windows.cpp.
namespace eacp::TCP::detail
{

// An int fd on POSIX, a SOCKET on Windows; both narrow to -1 when invalid.
using NativeSocket = std::intptr_t;
inline constexpr NativeSocket invalidSocket = -1;

// Resolves address and connects within connectTimeout, then arms the socket
// so each later send/receive obeys ioTimeout. Throws TCP::Error on failure.
NativeSocket socketConnect(const Address& address,
                           Time::MS connectTimeout,
                           Time::MS ioTimeout);

// A no-op on invalidSocket.
void socketClose(NativeSocket socket) noexcept;

// Writes once, returning the bytes accepted (always > 0). Throws TCP::Error on
// timeout or failure.
std::size_t socketSend(NativeSocket socket, const char* data, std::size_t length);

// Reads once; 0 means the peer closed cleanly. Throws TCP::Error on timeout or
// failure.
std::size_t socketReceive(NativeSocket socket, char* buffer, std::size_t length);

// Opens a listening socket on port (0 picks an ephemeral one), writing the
// actually-bound port back to boundPort. Throws TCP::Error on failure.
NativeSocket
    socketListen(std::uint16_t port, std::uint16_t& boundPort, BindInterface bindTo);

// Blocks up to acceptTimeout, arms the accepted socket with ioTimeout and
// writes the peer's address to peer. Throws TCP::Error on timeout or failure.
NativeSocket socketAccept(NativeSocket listenSocket,
                          Time::MS acceptTimeout,
                          Time::MS ioTimeout,
                          Address& peer);

} // namespace eacp::TCP::detail

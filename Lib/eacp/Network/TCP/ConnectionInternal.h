#pragma once

#include "Connection.h"

// The platform-specific socket backend, kept behind this header so the
// buffering and looping logic in Connection.cpp stays platform-free. POSIX
// (macOS + Linux) and Winsock implementations live in Connection-Posix.cpp
// and Connection-Windows.cpp respectively.
namespace eacp::TCP::detail
{

// A native socket handle held platform-agnostically: an int fd on POSIX, a
// SOCKET on Windows. Both compare equal to -1 when invalid once narrowed to
// intptr_t, so a single sentinel covers every platform.
using NativeSocket = std::intptr_t;
inline constexpr NativeSocket invalidSocket = -1;

// Resolves address and connects within connectTimeout, then arms the socket
// so each later send/receive obeys ioTimeout. Throws TCP::Error on failure.
NativeSocket socketConnect(const Address& address,
                           Time::MS connectTimeout,
                           Time::MS ioTimeout);

// Closes the handle. A no-op on invalidSocket, so it is always safe to call.
void socketClose(NativeSocket socket) noexcept;

// Writes once, returning the number of bytes accepted (always > 0). Throws
// TCP::Error on timeout or failure.
std::size_t socketSend(NativeSocket socket, const char* data, std::size_t length);

// Reads once into buffer, returning the byte count; 0 means the peer closed
// the stream cleanly. Throws TCP::Error on timeout or failure.
std::size_t socketReceive(NativeSocket socket, char* buffer, std::size_t length);

// Opens a listening socket on port (0 picks an ephemeral one), writing the
// actually-bound port back to boundPort. Throws TCP::Error on failure.
NativeSocket
    socketListen(std::uint16_t port, std::uint16_t& boundPort, BindInterface bindTo);

// Waits up to acceptTimeout for an inbound connection, accepts it, arms it
// with ioTimeout and writes the peer's address to peer. Throws TCP::Error on
// timeout or failure.
NativeSocket socketAccept(NativeSocket listenSocket,
                          Time::MS acceptTimeout,
                          Time::MS ioTimeout,
                          Address& peer);

} // namespace eacp::TCP::detail

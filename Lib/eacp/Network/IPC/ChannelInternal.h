#pragma once

#include "Channel.h"

// The platform-specific endpoint backend, kept behind this header so the
// naming, retry and buffering logic in Channel.cpp stays platform-free.
// POSIX (macOS, iOS and Linux, AF_UNIX sockets) and Win32 (named pipes)
// implementations live in Channel-Posix.cpp and Channel-Windows.cpp
// respectively. Every function below takes a name already folded by
// foldToFileName().
namespace eacp::IPC::detail
{

// A native endpoint handle held platform-agnostically: an int fd on POSIX, a
// HANDLE on Windows. Both compare equal to -1 when invalid once narrowed to
// intptr_t, so a single sentinel covers every platform.
using NativeChannel = std::intptr_t;
inline constexpr NativeChannel invalidChannel = -1;

// Makes one connection attempt. invalidChannel means nobody is serving the
// name right now - the caller owns the retry loop. Throws IPC::Error when
// the attempt itself failed.
NativeChannel channelTryConnect(const std::string& safeName);

// Claims the endpoint and starts listening. The caller must hold the server
// lock for the name: that is what makes it safe to sweep aside a crashed
// predecessor's leftover endpoint. Throws IPC::Error on failure.
NativeChannel channelBind(const std::string& safeName);

// Waits up to timeout (forever when zero or negative) for an inbound client
// and returns the connected endpoint, or invalidChannel when the timeout
// elapsed first. listener is taken by reference because Windows replaces
// the pipe instance on every accept; safeName feeds that replacement and
// POSIX ignores it. Throws IPC::Error on failure.
NativeChannel channelAccept(NativeChannel& listener,
                            const std::string& safeName,
                            Time::MS timeout);

// Writes once, returning the number of bytes accepted (always > 0). Throws
// IPC::Error on failure, including a peer that is gone.
std::size_t channelSend(NativeChannel channel, const char* data, std::size_t length);

// Reads once into buffer, returning the byte count; 0 means the peer closed
// the stream cleanly. Throws IPC::Error on failure.
std::size_t channelReceive(NativeChannel channel, char* buffer, std::size_t length);

// Wakes I/O blocked on channel from another thread; a woken receive reports
// a clean end of stream. POSIX shuts the socket down, which is permanent -
// every current and future receive ends immediately. Windows cancels only
// the calls already in flight, so a teardown loop repeats this until its
// reader has acknowledged.
void channelCancel(NativeChannel channel) noexcept;

// Closes a connected endpoint. A no-op on invalidChannel, so it is always
// safe to call.
void channelClose(NativeChannel channel) noexcept;

// Closes the listening endpoint and retires its name (POSIX unlinks the
// socket file; the caller still holds the server lock, so no successor can
// be mid-bind). A no-op on invalidChannel.
void channelServerClose(NativeChannel listener,
                        const std::string& safeName) noexcept;

} // namespace eacp::IPC::detail

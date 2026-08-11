#pragma once

#include "Channel.h"

#include <eacp/Core/Utils/FilePath.h>

// The platform endpoint backend: AF_UNIX sockets on POSIX, named pipes on
// Win32. Every function below takes a name already folded by foldToFileName().
namespace eacp::IPC::detail
{

// An int fd on POSIX, a HANDLE on Windows; both narrow to -1 when invalid.
using NativeChannel = std::intptr_t;
inline constexpr NativeChannel invalidChannel = -1;

// Per-user directory the AF_UNIX endpoints live in. POSIX only, and per-OS,
// but always short enough for sun_path and owned by this user.
FilePath channelRoot();

// One connection attempt. invalidChannel means nobody is serving the name
// right now - the caller owns the retry loop. Throws IPC::Error on failure.
NativeChannel channelTryConnect(const std::string& safeName);

// Claims the endpoint and starts listening. The caller must hold the server
// lock for the name. Throws IPC::Error on failure.
NativeChannel channelBind(const std::string& safeName);

// Blocks up to timeout (forever when zero or negative), answering
// invalidChannel when it elapses first. listener is by reference because
// Windows replaces the pipe instance on every accept; POSIX ignores safeName.
NativeChannel channelAccept(NativeChannel& listener,
                            const std::string& safeName,
                            Time::MS timeout);

// Writes once, returning the bytes accepted (always > 0). Throws IPC::Error
// on failure, including a peer that is gone.
std::size_t channelSend(NativeChannel channel, const char* data, std::size_t length);

// Reads once; 0 means the peer closed cleanly. Throws IPC::Error on failure.
std::size_t channelReceive(NativeChannel channel, char* buffer, std::size_t length);

// Wakes I/O blocked on channel from another thread; a woken receive reports a
// clean end of stream. Permanent on POSIX, one-shot on Windows, so a teardown
// loop repeats this until its reader has acknowledged.
void channelCancel(NativeChannel channel) noexcept;

// A no-op on invalidChannel.
void channelClose(NativeChannel channel) noexcept;

// Closes the listener and retires its name (POSIX unlinks the socket file).
// A no-op on invalidChannel.
void channelServerClose(NativeChannel listener,
                        const std::string& safeName) noexcept;

} // namespace eacp::IPC::detail

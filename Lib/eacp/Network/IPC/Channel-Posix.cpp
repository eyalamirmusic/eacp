#include "ChannelInternal.h"

#include <eacp/Core/Utils/StdPath.h>

#include <fcntl.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/un.h>
#include <unistd.h>

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <system_error>

namespace eacp::IPC::detail
{
namespace
{
[[noreturn]] void fail(const std::string& context)
{
    throw Error(context + ": " + std::strerror(errno));
}

// FD_CLOEXEC is load-bearing, as it is for the lock: a child inheriting the
// descriptor would keep the stream (or the listening endpoint) alive after
// its parent died, and eacp spawns children. SO_NOSIGPIPE turns a macOS
// send-to-dead-peer into an error return instead of a process-killing
// signal; Linux spells the same thing MSG_NOSIGNAL on each send.
void configureDescriptor(int fd)
{
    ::fcntl(fd, F_SETFD, FD_CLOEXEC);

#ifdef SO_NOSIGPIPE
    auto on = 1;
    ::setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &on, sizeof(on));
#endif

    // The default AF_UNIX buffers (8KB on macOS) chop a multi-megabyte
    // message into hundreds of send/recv round trips, a context switch
    // each; a wider window moves it in a handful. Best effort - the
    // kernel clamps what it won't grant.
    auto bufferSize = 1 << 20;
    ::setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &bufferSize, sizeof(bufferSize));
    ::setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &bufferSize, sizeof(bufferSize));
}

int sendFlags()
{
#ifdef MSG_NOSIGNAL
    return MSG_NOSIGNAL;
#else
    return 0;
#endif
}

FilePath channelRoot()
{
#if defined(__APPLE__)
    // The per-user temporary directory, not appDataDirectory: sun_path caps
    // a socket path at 104 bytes here, and the lock's no-reaper concern
    // doesn't apply - a swept endpoint is reclaimed on the next bind.
    return FilePath::tempDirectory() / "eacp.channels";
#else
    // The XDG runtime directory is the canonical per-user socket home on
    // Linux. The /tmp fallback is shared between users, so the directory
    // name carries the uid and ownership is verified below.
    if (auto* runtime = std::getenv("XDG_RUNTIME_DIR");
        runtime != nullptr && runtime[0] != '\0')
        return FilePath {runtime} / "eacp.channels";

    return FilePath::tempDirectory()
           / ("eacp.channels-" + std::to_string(::getuid()));
#endif
}

// Resolves (and when asked, creates) the directory the endpoints live in,
// refusing one this user does not own: a socket in a directory someone else
// controls is a connection someone else can intercept.
FilePath channelDirectory(bool create)
{
    auto root = channelRoot();

    if (root.empty())
        throw Error("cannot resolve a directory for channel endpoints");

    if (create)
    {
        auto failure = std::error_code {};
        std::filesystem::create_directories(toStdPath(root), failure);

        if (failure)
            throw Error("cannot create '" + root.str() + "': " + failure.message());
    }

    struct ::stat info = {};

    if (::lstat(root.c_str(), &info) != 0)
    {
        // Nothing there yet reads as "no server" on the connect side.
        if (errno == ENOENT && !create)
            return root;

        fail("cannot inspect '" + root.str() + "'");
    }

    if (!S_ISDIR(info.st_mode) || info.st_uid != ::getuid())
        throw Error("'" + root.str() + "' is not a directory owned by this user");

    if (create)
        ::chmod(root.c_str(), 0700);

    return root;
}

std::string endpointPath(const std::string& safeName, bool create)
{
    auto path = (channelDirectory(create) / (safeName + ".sock")).str();

    if (path.size() >= sizeof(sockaddr_un::sun_path))
        throw Error("channel name '" + safeName
                    + "' makes a socket path longer than the platform allows "
                      "- use a shorter name");

    return path;
}

sockaddr_un toAddress(const std::string& path)
{
    auto address = sockaddr_un {};
    address.sun_family = AF_UNIX;
    std::memcpy(address.sun_path, path.c_str(), path.size());
    return address;
}
} // namespace

NativeChannel channelTryConnect(const std::string& safeName)
{
    auto path = endpointPath(safeName, false);
    auto fd = ::socket(AF_UNIX, SOCK_STREAM, 0);

    if (fd < 0)
        fail("cannot create a socket");

    configureDescriptor(fd);
    auto address = toAddress(path);

    if (::connect(fd, (sockaddr*) &address, sizeof(address)) == 0)
        return fd;

    auto reason = errno;
    ::close(fd);

    // Nobody serving yet: no endpoint, or a bound-but-dead one. The portable
    // retry loop owns what happens next.
    if (reason == ENOENT || reason == ECONNREFUSED)
        return invalidChannel;

    errno = reason;
    fail("cannot connect to channel '" + safeName + "'");
}

NativeChannel channelBind(const std::string& safeName)
{
    auto path = endpointPath(safeName, true);
    auto fd = ::socket(AF_UNIX, SOCK_STREAM, 0);

    if (fd < 0)
        fail("cannot create a socket");

    configureDescriptor(fd);

    // A leftover endpoint here is always a corpse: the caller holds the
    // server lock, so no live server owns this name.
    ::unlink(path.c_str());

    auto address = toAddress(path);

    if (::bind(fd, (sockaddr*) &address, sizeof(address)) < 0)
    {
        auto reason = errno;
        ::close(fd);
        errno = reason;
        fail("cannot bind channel '" + safeName + "'");
    }

    ::chmod(path.c_str(), 0600);

    if (::listen(fd, SOMAXCONN) < 0)
    {
        auto reason = errno;
        ::close(fd);
        errno = reason;
        fail("cannot listen on channel '" + safeName + "'");
    }

    return fd;
}

NativeChannel
    channelAccept(NativeChannel& listener, const std::string&, Time::MS timeout)
{
    auto fd = (int) listener;

    for (;;)
    {
        auto readable = fd_set {};
        FD_ZERO(&readable);
        FD_SET(fd, &readable);

        auto tv = timeval {};
        tv.tv_sec = (time_t) (timeout.count / 1000);
        tv.tv_usec = (suseconds_t) ((timeout.count % 1000) * 1000);
        auto* deadline = timeout.count > 0 ? &tv : nullptr; // null = forever

        auto ready = ::select(fd + 1, &readable, nullptr, nullptr, deadline);

        if (ready == 0)
            return invalidChannel;

        if (ready < 0)
        {
            if (errno == EINTR)
                continue; // restarts the full wait; signals here are rare

            fail("cannot wait for a channel client");
        }

        auto accepted = ::accept(fd, nullptr, nullptr);

        if (accepted < 0)
        {
            if (errno == EINTR || errno == ECONNABORTED)
                continue;

            fail("cannot accept a channel client");
        }

        configureDescriptor(accepted);
        return accepted;
    }
}

std::size_t channelSend(NativeChannel channel, const char* data, std::size_t length)
{
    auto sent = ::send((int) channel, data, length, sendFlags());

    if (sent < 0)
        fail("cannot send on channel");

    return (std::size_t) sent;
}

std::size_t channelReceive(NativeChannel channel, char* buffer, std::size_t length)
{
    auto received = ::recv((int) channel, buffer, length, 0);

    if (received < 0)
        fail("cannot receive on channel");

    return (std::size_t) received;
}

void channelCancel(NativeChannel channel) noexcept
{
    // Permanent by design: every blocked and future recv on this socket
    // returns 0, so a teardown can never lose the race with its reader.
    if (channel != invalidChannel)
        ::shutdown((int) channel, SHUT_RDWR);
}

void channelClose(NativeChannel channel) noexcept
{
    if (channel != invalidChannel)
        ::close((int) channel);
}

void channelServerClose(NativeChannel listener, const std::string& safeName) noexcept
{
    if (listener == invalidChannel)
        return;

    ::close((int) listener);

    // Retiring the file makes the next connect() fail fast with "nobody
    // here" instead of a dangling refusal. Still under the server lock, so
    // no successor can be mid-bind. Swallows everything: destructors land
    // here.
    try
    {
        ::unlink(endpointPath(safeName, false).c_str());
    }
    catch (...)
    {
    }
}

} // namespace eacp::IPC::detail

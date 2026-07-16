#include "ChannelInternal.h"

#include <eacp/Core/Utils/WinInclude.h>

#include <aclapi.h>

#include <algorithm>

namespace eacp::IPC::detail
{
namespace
{
constexpr auto pipeBufferSize = DWORD {65536};
constexpr auto acceptPollInterval = Time::MS {25};

std::wstring widen(const std::string& text)
{
    if (text.empty())
        return {};

    auto length = ::MultiByteToWideChar(
        CP_UTF8, 0, text.c_str(), (int) text.size(), nullptr, 0);

    if (length <= 0)
        throw Error("cannot convert '" + text + "' to UTF-16");

    auto wide = std::wstring((std::size_t) length, L'\0');
    ::MultiByteToWideChar(
        CP_UTF8, 0, text.c_str(), (int) text.size(), wide.data(), length);
    return wide;
}

[[noreturn]] void fail(const std::string& context)
{
    throw Error(context + ": Windows error " + std::to_string(::GetLastError()));
}

std::wstring pipePath(const std::string& safeName)
{
    return widen("\\\\.\\pipe\\eacp.channels." + safeName);
}

// Pipe names share one machine-global namespace with no directory to guard
// them, so this DACL is the only thing keeping another user off the
// endpoint: it grants this user alone, which also reserves further instance
// creation (squatting the name needs FILE_CREATE_PIPE_INSTANCE) to us.
class PipeSecurity
{
public:
    PipeSecurity()
    {
        auto token = HANDLE {};

        if (::OpenProcessToken(::GetCurrentProcess(), TOKEN_QUERY, &token) == 0)
            fail("cannot open the process token");

        auto size = DWORD {0};

        if (::GetTokenInformation(token, TokenUser, user, sizeof(user), &size) == 0)
        {
            ::CloseHandle(token);
            fail("cannot resolve the current user");
        }

        ::CloseHandle(token);

        auto access = EXPLICIT_ACCESSW {};
        access.grfAccessPermissions = GENERIC_READ | GENERIC_WRITE;
        access.grfAccessMode = SET_ACCESS;
        access.grfInheritance = NO_INHERITANCE;
        access.Trustee.TrusteeForm = TRUSTEE_IS_SID;
        access.Trustee.TrusteeType = TRUSTEE_IS_USER;
        access.Trustee.ptstrName = (LPWCH) ((TOKEN_USER*) user)->User.Sid;

        if (::SetEntriesInAclW(1, &access, nullptr, &acl) != ERROR_SUCCESS)
            fail("cannot build the channel ACL");

        ::InitializeSecurityDescriptor(&descriptor, SECURITY_DESCRIPTOR_REVISION);
        ::SetSecurityDescriptorDacl(&descriptor, TRUE, acl, FALSE);

        attributes.nLength = sizeof(attributes);
        attributes.lpSecurityDescriptor = &descriptor;
        attributes.bInheritHandle = FALSE; // matches FD_CLOEXEC on POSIX
    }

    ~PipeSecurity()
    {
        if (acl != nullptr)
            ::LocalFree(acl);
    }

    PipeSecurity(const PipeSecurity&) = delete;
    PipeSecurity& operator=(const PipeSecurity&) = delete;

    SECURITY_ATTRIBUTES* get() { return &attributes; }

private:
    alignas(TOKEN_USER) BYTE user[sizeof(TOKEN_USER) + SECURITY_MAX_SID_SIZE] = {};
    PACL acl = nullptr;
    SECURITY_DESCRIPTOR descriptor = {};
    SECURITY_ATTRIBUTES attributes = {};
};

NativeChannel createInstance(const std::string& safeName, bool first)
{
    auto security = PipeSecurity {};

    // FILE_FLAG_FIRST_PIPE_INSTANCE turns a squatted name into an error on
    // the first instance instead of a confusing split-brain pipe.
    auto open =
        (DWORD) (PIPE_ACCESS_DUPLEX | (first ? FILE_FLAG_FIRST_PIPE_INSTANCE : 0));

    auto pipe = ::CreateNamedPipeW(pipePath(safeName).c_str(),
                                   open,
                                   PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT
                                       | PIPE_REJECT_REMOTE_CLIENTS,
                                   PIPE_UNLIMITED_INSTANCES,
                                   pipeBufferSize,
                                   pipeBufferSize,
                                   0,
                                   security.get());

    if (pipe == INVALID_HANDLE_VALUE)
        fail("cannot create channel '" + safeName + "'");

    return (NativeChannel) (std::intptr_t) pipe;
}

void setPipeMode(HANDLE pipe, DWORD mode)
{
    ::SetNamedPipeHandleState(pipe, &mode, nullptr, nullptr);
}

// Waits for a client on the pipe instance. Blocking mode covers the
// wait-forever case at zero cost; a bounded wait flips the instance to
// no-wait mode and polls, the same trade ScopedLock makes (a timed
// ConnectNamedPipe needs overlapped I/O, which would tax every later read
// and write on the handle).
bool waitForClient(HANDLE pipe, Time::MS timeout)
{
    if (timeout.count <= 0)
    {
        if (::ConnectNamedPipe(pipe, nullptr) != 0
            || ::GetLastError() == ERROR_PIPE_CONNECTED)
            return true; // CONNECTED means the client beat us to the wait

        fail("cannot wait for a channel client");
    }

    setPipeMode(pipe, PIPE_READMODE_BYTE | PIPE_NOWAIT);

    auto deadline = Time::Deadline {timeout};
    auto connected = false;

    for (;;)
    {
        if (::ConnectNamedPipe(pipe, nullptr) != 0
            || ::GetLastError() == ERROR_PIPE_CONNECTED)
        {
            connected = true;
            break;
        }

        if (::GetLastError() != ERROR_PIPE_LISTENING)
        {
            setPipeMode(pipe, PIPE_READMODE_BYTE | PIPE_WAIT);
            fail("cannot wait for a channel client");
        }

        if (deadline.expired())
            break;

        auto remaining = deadline.remaining();
        Time::sleep(remaining < acceptPollInterval ? remaining : acceptPollInterval);
    }

    setPipeMode(pipe, PIPE_READMODE_BYTE | PIPE_WAIT);
    return connected;
}
} // namespace

NativeChannel channelTryConnect(const std::string& safeName)
{
    // SECURITY_IDENTIFICATION keeps a server from borrowing this client's
    // identity wholesale; identification is all a local peer needs.
    auto handle = ::CreateFileW(pipePath(safeName).c_str(),
                                GENERIC_READ | GENERIC_WRITE,
                                0,
                                nullptr,
                                OPEN_EXISTING,
                                SECURITY_SQOS_PRESENT | SECURITY_IDENTIFICATION,
                                nullptr);

    if (handle != INVALID_HANDLE_VALUE)
        return (NativeChannel) (std::intptr_t) handle;

    auto reason = ::GetLastError();

    // No pipe, or no free instance right now: both read as "not yet" and
    // feed the portable retry loop.
    if (reason == ERROR_FILE_NOT_FOUND || reason == ERROR_PIPE_BUSY)
        return invalidChannel;

    fail("cannot connect to channel '" + safeName + "'");
}

NativeChannel channelBind(const std::string& safeName)
{
    return createInstance(safeName, true);
}

NativeChannel channelAccept(NativeChannel& listener,
                            const std::string& safeName,
                            Time::MS timeout)
{
    if (!waitForClient((HANDLE) listener, timeout))
        return invalidChannel;

    // The connected instance is the channel; a fresh instance takes over
    // listening duty. This is the named-pipe shape of accept(): a listener
    // is a pipe instance, not a factory handle.
    auto connected = listener;
    listener = createInstance(safeName, false);
    return connected;
}

std::size_t channelSend(NativeChannel channel, const char* data, std::size_t length)
{
    auto toWrite = (DWORD) std::min<std::size_t>(length, MAXDWORD);
    auto written = DWORD {0};

    if (::WriteFile((HANDLE) channel, data, toWrite, &written, nullptr) == 0)
        fail("cannot send on channel");

    return written;
}

std::size_t channelReceive(NativeChannel channel, char* buffer, std::size_t length)
{
    auto toRead = (DWORD) std::min<std::size_t>(length, MAXDWORD);
    auto received = DWORD {0};

    if (::ReadFile((HANDLE) channel, buffer, toRead, &received, nullptr) == 0)
    {
        auto reason = ::GetLastError();

        // BROKEN_PIPE is the peer closing cleanly, same as a POSIX EOF;
        // OPERATION_ABORTED is channelCancel waking a teardown's reader.
        // Both mean this stream is over.
        if (reason == ERROR_BROKEN_PIPE || reason == ERROR_OPERATION_ABORTED)
            return 0;

        fail("cannot receive on channel");
    }

    return received;
}

void channelCancel(NativeChannel channel) noexcept
{
    // One-shot: only I/O already in flight is cancelled, so callers loop
    // this until their reader thread acknowledges (see the seam comment).
    if (channel != invalidChannel)
        ::CancelIoEx((HANDLE) channel, nullptr);
}

void channelClose(NativeChannel channel) noexcept
{
    if (channel == invalidChannel)
        return;

    // Unlike a socket, a pipe may drop written-but-unread bytes when its
    // handle closes; the flush holds the door until the peer has read them.
    // A vanished peer fails the flush immediately, so this cannot hang on
    // the dead.
    ::FlushFileBuffers((HANDLE) channel);
    ::CloseHandle((HANDLE) channel);
}

void channelServerClose(NativeChannel listener, const std::string&) noexcept
{
    if (listener != invalidChannel)
        ::CloseHandle((HANDLE) listener);
}

} // namespace eacp::IPC::detail

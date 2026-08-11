#include "ChannelInternal.h"

#include <eacp/Core/Utils/WinInclude.h>

#include <aclapi.h>

#include <algorithm>

namespace eacp::IPC::detail
{
namespace
{
// Advisory quota; an undersized one forces writer/reader ping-pong on the
// bulk payloads (megabyte video frames) this carries.
constexpr auto pipeBufferSize = DWORD {1} << 20;

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

[[noreturn]] void fail(const std::string& context, DWORD reason)
{
    throw Error(context + ": Windows error " + std::to_string(reason));
}

[[noreturn]] void fail(const std::string& context)
{
    fail(context, ::GetLastError());
}

// Every endpoint is opened FILE_FLAG_OVERLAPPED, so each operation waits on
// its own event. Without the flag the I/O manager serialises operations on the
// handle, and a send behind a parked read deadlocks.
class Operation
{
public:
    Operation()
    {
        overlapped.hEvent = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);

        if (overlapped.hEvent == nullptr)
            fail("cannot create a channel I/O event");
    }

    ~Operation() { ::CloseHandle(overlapped.hEvent); }

    Operation(const Operation&) = delete;
    Operation& operator=(const Operation&) = delete;

    OVERLAPPED* get() { return &overlapped; }
    HANDLE event() const { return overlapped.hEvent; }

private:
    OVERLAPPED overlapped = {};
};

// Blocks until an operation already handed to the kernel finishes, leaving the
// caller to judge which failure reasons are just the stream ending.
bool completed(HANDLE pipe, Operation& operation, DWORD& transferred, DWORD& reason)
{
    if (::GetOverlappedResult(pipe, operation.get(), &transferred, TRUE) != 0)
        return true;

    reason = ::GetLastError();
    return false;
}

std::wstring pipePath(const std::string& safeName)
{
    return widen("\\\\.\\pipe\\eacp.channels." + safeName);
}

// Pipe names share one machine-global namespace, so this DACL granting only
// the current user is the only thing keeping another user off the endpoint,
// and it reserves further instance creation to us too.
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
    auto open = (DWORD) (PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED
                         | (first ? FILE_FLAG_FIRST_PIPE_INSTANCE : 0));

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

// ConnectNamedPipe reports "no wait to perform" by failing. CONNECTED is a
// client still there; NO_DATA one that hung up, still a connection because the
// instance holds whatever it wrote - refusing it would lose those bytes.
bool clientArrived(DWORD reason)
{
    return reason == ERROR_PIPE_CONNECTED || reason == ERROR_NO_DATA;
}

// BROKEN_PIPE is the peer closing cleanly (a POSIX EOF), NO_DATA one that
// closed with the connect unanswered, OPERATION_ABORTED a channelCancel.
bool endOfStream(DWORD reason)
{
    return reason == ERROR_BROKEN_PIPE || reason == ERROR_NO_DATA
           || reason == ERROR_OPERATION_ABORTED;
}

// Blocks up to timeout on the overlapped connect's own event, so a bounded
// accept costs one wait rather than a polling loop.
bool waitForClient(HANDLE pipe, Time::MS timeout)
{
    auto operation = Operation {};

    // An overlapped ConnectNamedPipe never succeeds outright: it either goes
    // pending or reports why it had nothing to wait for.
    if (::ConnectNamedPipe(pipe, operation.get()) != 0)
        return true;

    auto reason = ::GetLastError();

    if (clientArrived(reason))
        return true;

    if (reason != ERROR_IO_PENDING)
        fail("cannot wait for a channel client", reason);

    auto wait = timeout.count <= 0 ? INFINITE : (DWORD) timeout.count;

    if (::WaitForSingleObject(operation.event(), wait) == WAIT_OBJECT_0)
    {
        auto transferred = DWORD {0};

        if (completed(pipe, operation, transferred, reason))
            return true;

        if (clientArrived(reason))
            return true;

        fail("cannot wait for a channel client", reason);
    }

    // The in-flight connect's OVERLAPPED lives on this stack, so it must be
    // called off and reaped or the kernel writes into a dead frame. A client
    // that outruns the cancel is kept.
    ::CancelIoEx(pipe, operation.get());

    auto transferred = DWORD {0};

    if (completed(pipe, operation, transferred, reason))
        return true;

    return clientArrived(reason);
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
                                FILE_FLAG_OVERLAPPED | SECURITY_SQOS_PRESENT
                                    | SECURITY_IDENTIFICATION,
                                nullptr);

    if (handle != INVALID_HANDLE_VALUE)
        return (NativeChannel) (std::intptr_t) handle;

    auto reason = ::GetLastError();

    // No pipe, or no free instance right now: both read as "not yet".
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

    // A listener is a pipe instance, not a factory handle: the connected one
    // becomes the channel and a fresh instance takes over listening.
    auto connected = listener;
    listener = createInstance(safeName, false);
    return connected;
}

std::size_t channelSend(NativeChannel channel, const char* data, std::size_t length)
{
    auto pipe = (HANDLE) channel;
    auto toWrite = (DWORD) std::min<std::size_t>(length, MAXDWORD);
    auto operation = Operation {};
    auto written = DWORD {0};

    if (::WriteFile(pipe, data, toWrite, &written, operation.get()) != 0)
        return written;

    auto reason = ::GetLastError();

    if (reason != ERROR_IO_PENDING)
        fail("cannot send on channel", reason);

    if (!completed(pipe, operation, written, reason))
        fail("cannot send on channel", reason);

    return written;
}

std::size_t channelReceive(NativeChannel channel, char* buffer, std::size_t length)
{
    auto pipe = (HANDLE) channel;
    auto toRead = (DWORD) std::min<std::size_t>(length, MAXDWORD);
    auto operation = Operation {};
    auto received = DWORD {0};

    if (::ReadFile(pipe, buffer, toRead, &received, operation.get()) != 0)
        return received;

    auto reason = ::GetLastError();

    if (reason == ERROR_IO_PENDING && completed(pipe, operation, received, reason))
        return received;

    if (endOfStream(reason))
        return 0;

    fail("cannot receive on channel", reason);
}

void channelCancel(NativeChannel channel) noexcept
{
    // One-shot: only I/O already in flight is cancelled, so callers loop this
    // until their reader thread acknowledges.
    if (channel != invalidChannel)
        ::CancelIoEx((HANDLE) channel, nullptr);
}

void channelClose(NativeChannel channel) noexcept
{
    if (channel == invalidChannel)
        return;

    // Unlike a socket, a pipe may drop written-but-unread bytes on close; the
    // flush waits for the peer to read them, and fails fast if it is gone.
    ::FlushFileBuffers((HANDLE) channel);
    ::CloseHandle((HANDLE) channel);
}

void channelServerClose(NativeChannel listener, const std::string&) noexcept
{
    if (listener != invalidChannel)
        ::CloseHandle((HANDLE) listener);
}

} // namespace eacp::IPC::detail

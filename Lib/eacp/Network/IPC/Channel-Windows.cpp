#include "ChannelInternal.h"

#include <eacp/Core/Utils/WinInclude.h>

#include <aclapi.h>

#include <algorithm>

namespace eacp::IPC::detail
{
namespace
{
constexpr auto pipeBufferSize = DWORD {65536};

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

// Every endpoint here is opened FILE_FLAG_OVERLAPPED, so each operation
// carries one of these and waits on its own event.
//
// The flag is not an optimisation, it is what makes Channel's promise -
// one thread may send while another receives - true on Windows. A handle
// without it is synchronous, and the I/O manager serialises every
// operation on a synchronous handle: a reader parked in ReadFile owns the
// file object until it completes, so a send from another thread waits
// behind a read that is itself waiting for the peer. Nothing breaks that
// cycle, and a Messenger (whose reader is always parked) deadlocks on its
// first send.
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

// Blocks until an operation already handed to the kernel finishes, and
// reports the bytes it moved. Fails with the reason, leaving the caller to
// decide which reasons are news and which are just the stream ending.
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

// A client is on the instance already, so ConnectNamedPipe had no wait to
// perform and says so by failing. CONNECTED is one that is still there;
// NO_DATA is one that has already hung up, which is a connection all the
// same - the instance still holds whatever it wrote, and the stream ends
// once that is drained. Refusing NO_DATA would lose a short-lived client's
// bytes and turn the POSIX shape of this - a connection waits in the
// backlog whether or not its dialer is still around - into an error.
bool clientArrived(DWORD reason)
{
    return reason == ERROR_PIPE_CONNECTED || reason == ERROR_NO_DATA;
}

// BROKEN_PIPE is the peer closing cleanly, same as a POSIX EOF; NO_DATA is
// a peer that closed with the connect still unanswered. OPERATION_ABORTED
// is channelCancel waking a teardown's reader. All mean this stream is
// over.
bool endOfStream(DWORD reason)
{
    return reason == ERROR_BROKEN_PIPE || reason == ERROR_NO_DATA
           || reason == ERROR_OPERATION_ABORTED;
}

// Waits up to timeout for a client on the pipe instance. The wait is the
// overlapped connect's own event, so a bounded accept costs one wait
// rather than a polling loop.
bool waitForClient(HANDLE pipe, Time::MS timeout)
{
    auto operation = Operation {};

    // An overlapped ConnectNamedPipe never succeeds outright: it either
    // goes pending or reports why it had nothing to wait for.
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

    // The connect is still in flight and its OVERLAPPED lives on this
    // stack, so it has to be called off and then reaped - dropping it here
    // would leave the kernel writing into a dead frame. A client that
    // landed in that same breath outruns the cancel and is kept, rather
    // than being dropped for being a moment late.
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

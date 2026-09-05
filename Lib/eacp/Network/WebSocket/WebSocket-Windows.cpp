#include "Backend.h"

#include <eacp/Core/Utils/Strings.h>
#include <eacp/Core/Utils/WinInclude.h>

#include <winhttp.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

// WinHTTP's WebSocket API (Windows 8 and later) does the framing, the masking
// and the pong answers itself, so this backend is a handshake, a receive loop
// on a thread of its own, and the closing exchange. Every file-scope name here
// is prefixed, the library being one translation unit under a unity build.
namespace eacp::WebSocket
{

namespace
{

constexpr auto webSocketSwitchingProtocols = 101;
constexpr auto webSocketReceiveChunkSize = std::size_t {64 * 1024};
constexpr auto webSocketErrorTextLength = std::size_t {512};
constexpr auto webSocketMaxCloseReasonLength = std::size_t {123};
constexpr auto webSocketDefaultTimeoutMilliseconds = std::int64_t {30000};
constexpr auto webSocketKeepAliveMilliseconds = DWORD {30000};

// RFC 6455's close codes. 1005 and 1006 are the two a peer may never put on
// the wire, so an echo of either goes out as a plain 1000.
constexpr USHORT webSocketNormalClose = 1000;
constexpr USHORT webSocketEmptyClose = 1005;
constexpr USHORT webSocketAbnormalClose = 1006;
constexpr USHORT webSocketMessageTooBigClose = 1009;

// WinHTTP's own error strings live in winhttp.dll's message table rather than
// the system's, so search both - the same lookup the HTTP backend does.
std::string webSocketErrorMessage(DWORD code, const std::string& what)
{
    wchar_t text[webSocketErrorTextLength] {};
    FormatMessageW(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_FROM_HMODULE
                       | FORMAT_MESSAGE_IGNORE_INSERTS,
                   GetModuleHandleW(L"winhttp.dll"),
                   code,
                   0,
                   text,
                   static_cast<DWORD>(webSocketErrorTextLength),
                   nullptr);

    auto message = Strings::trim(Strings::narrow(text));

    if (message.empty())
        message = what + " failed (error " + std::to_string(code) + ")";

    return message;
}

[[noreturn]] void webSocketThrowLastError(const std::string& what)
{
    throw std::runtime_error(webSocketErrorMessage(GetLastError(), what));
}

// One process-wide session, never closed: WinHTTP session handles are
// thread-safe and cheap to share. The fallback covers systems predating
// automatic proxy discovery.
HINTERNET webSocketSession()
{
    static auto handle = []
    {
        auto opened = WinHttpOpen(L"eacp",
                                  WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                                  WINHTTP_NO_PROXY_NAME,
                                  WINHTTP_NO_PROXY_BYPASS,
                                  0);
        if (!opened)
            opened = WinHttpOpen(L"eacp",
                                 WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                 WINHTTP_NO_PROXY_NAME,
                                 WINHTTP_NO_PROXY_BYPASS,
                                 0);
        return opened;
    }();

    return handle;
}

int webSocketBoundedTimeout(Time::MS timeout)
{
    auto milliseconds =
        timeout.count > 0 ? timeout.count : webSocketDefaultTimeoutMilliseconds;

    auto largest = static_cast<std::int64_t>(std::numeric_limits<int>::max());

    return static_cast<int>(milliseconds > largest ? largest : milliseconds);
}

bool webSocketHasScheme(const std::string& url, std::string_view scheme)
{
    if (url.size() < scheme.size())
        return false;

    return Strings::equalsCaseInsensitive(
        std::string_view {url}.substr(0, scheme.size()), scheme);
}

// WinHttpCrackUrl knows nothing of ws:// and wss://, so the scheme becomes the
// one it upgrades from; whether the transport is secure travels separately.
std::string webSocketAsHttpUrl(const std::string& url)
{
    if (webSocketHasScheme(url, "wss://"))
        return "https://" + url.substr(6);

    if (webSocketHasScheme(url, "ws://"))
        return "http://" + url.substr(5);

    return url;
}

struct WebSocketAddress
{
    std::wstring host;
    std::wstring pathWithQuery;
    INTERNET_PORT port = 0;
    bool secure = false;
};

WebSocketAddress webSocketCrackUrl(const std::string& url)
{
    if (url.empty())
        throw std::runtime_error("The WebSocket URL cannot be empty");

    auto wide = Strings::widen(webSocketAsHttpUrl(url));

    auto parts = URL_COMPONENTS {};
    parts.dwStructSize = sizeof(parts);
    parts.dwSchemeLength = static_cast<DWORD>(-1);
    parts.dwHostNameLength = static_cast<DWORD>(-1);
    parts.dwUrlPathLength = static_cast<DWORD>(-1);
    parts.dwExtraInfoLength = static_cast<DWORD>(-1);

    if (!WinHttpCrackUrl(wide.c_str(), 0, 0, &parts))
        webSocketThrowLastError("Parsing the WebSocket URL");

    auto address = WebSocketAddress {};
    address.host.assign(parts.lpszHostName, parts.dwHostNameLength);
    address.pathWithQuery.assign(parts.lpszUrlPath, parts.dwUrlPathLength);

    if (parts.lpszExtraInfo && parts.dwExtraInfoLength > 0)
        address.pathWithQuery.append(parts.lpszExtraInfo, parts.dwExtraInfoLength);

    if (address.pathWithQuery.empty())
        address.pathWithQuery = L"/";

    address.port = parts.nPort;
    address.secure = parts.nScheme == INTERNET_SCHEME_HTTPS;
    return address;
}

std::wstring webSocketRequestHeaders(const Options& options)
{
    auto lines = std::string();

    for (const auto& [key, value]: options.headers)
    {
        lines.append(key);
        lines.append(": ");
        lines.append(value);
        lines.append("\r\n");
    }

    if (!options.protocols.empty())
    {
        lines.append("Sec-WebSocket-Protocol: ");

        auto separator = std::string();

        for (const auto& protocol: options.protocols)
        {
            lines.append(separator);
            lines.append(protocol);
            separator = ", ";
        }

        lines.append("\r\n");
    }

    return Strings::widen(lines);
}

int webSocketQueryStatusCode(HINTERNET request)
{
    auto status = DWORD {0};
    auto size = DWORD {sizeof(status)};

    WinHttpQueryHeaders(request,
                        WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                        WINHTTP_HEADER_NAME_BY_INDEX,
                        &status,
                        &size,
                        WINHTTP_NO_HEADER_INDEX);

    return static_cast<int>(status);
}

std::string webSocketQueryHeader(HINTERNET request, const wchar_t* name)
{
    auto size = DWORD {0};
    WinHttpQueryHeaders(request,
                        WINHTTP_QUERY_CUSTOM,
                        name,
                        WINHTTP_NO_OUTPUT_BUFFER,
                        &size,
                        WINHTTP_NO_HEADER_INDEX);

    if (GetLastError() != ERROR_INSUFFICIENT_BUFFER || size == 0)
        return {};

    auto text = std::wstring(size / sizeof(wchar_t) + 1, L'\0');

    if (!WinHttpQueryHeaders(request,
                             WINHTTP_QUERY_CUSTOM,
                             name,
                             text.data(),
                             &size,
                             WINHTTP_NO_HEADER_INDEX))
        return {};

    return Strings::trim(Strings::narrow(text.c_str()));
}

bool webSocketBufferIsText(WINHTTP_WEB_SOCKET_BUFFER_TYPE type)
{
    return type == WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE
           || type == WINHTTP_WEB_SOCKET_UTF8_FRAGMENT_BUFFER_TYPE;
}

bool webSocketBufferCompletesMessage(WINHTTP_WEB_SOCKET_BUFFER_TYPE type)
{
    return type == WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE
           || type == WINHTTP_WEB_SOCKET_BINARY_MESSAGE_BUFFER_TYPE;
}

USHORT webSocketEchoableStatus(USHORT status)
{
    if (status == webSocketEmptyClose || status == webSocketAbnormalClose)
        return webSocketNormalClose;

    return status;
}

// The transport for one connection. The worker thread owns the handshake and
// the receive loop; send, close and the destructor arrive on the message
// thread, so the only concurrency is that thread against the worker: handles
// are swapped under handleMutex, the two calls that put bytes on the wire are
// serialised by sendMutex, and closing a handle is what cuts short whatever
// call the worker is sitting in.
class WebSocketTransport final : public Backend
{
public:
    WebSocketTransport(const std::string& urlToUse,
                       const Options& optionsToUse,
                       std::shared_ptr<Sink> sinkToUse)
        : url(urlToUse)
        , options(optionsToUse)
        , sink(std::move(sinkToUse))
    {
        worker = std::thread([this] { run(); });
    }

    ~WebSocketTransport() override
    {
        tearingDown.store(true);

        closeHandle(webSocketHandle);
        closeHandle(requestHandle);

        if (worker.joinable())
            worker.join();

        closeHandle(connectionHandle);
    }

    void send(const Message& message) override
    {
        if (terminated.load() || closeRequested.load())
            return;

        auto* socket = handleOf(webSocketHandle);

        if (!socket)
            return;

        auto type = message.type == MessageType::text
                        ? WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE
                        : WINHTTP_WEB_SOCKET_BINARY_MESSAGE_BUFFER_TYPE;

        auto* payload = const_cast<char*>(message.data.data());
        auto length = static_cast<DWORD>(message.data.size());

        auto result = DWORD {ERROR_SUCCESS};

        {
            auto lock = std::scoped_lock(sendMutex);
            result = WinHttpWebSocketSend(socket, type, payload, length);
        }

        if (result != ERROR_SUCCESS)
            reportFailed(webSocketErrorMessage(result, "Sending the message"));
    }

    void close(int code, const std::string& reason) override
    {
        closeRequested.store(true);

        auto* socket = handleOf(webSocketHandle);

        // Still handshaking: closing the request handle cuts the blocking
        // call short, and the worker reports the abandoned attempt.
        if (!socket)
        {
            closeHandle(requestHandle);
            return;
        }

        shutdown(socket, static_cast<USHORT>(code), reason);
    }

private:
    void run()
    {
        try
        {
            handshake();
        }
        catch (const std::exception& e)
        {
            reportHandshakeFailure(e.what());
            closeEveryHandle();
            return;
        }

        receiveLoop();
        closeEveryHandle();
    }

    void handshake()
    {
        throwIfAbandoned();

        if (!webSocketSession())
            webSocketThrowLastError("Opening the WinHTTP session");

        auto address = webSocketCrackUrl(url);

        auto* connection = WinHttpConnect(
            webSocketSession(), address.host.c_str(), address.port, 0);
        if (!connection)
            webSocketThrowLastError("Connecting");

        adoptHandle(connectionHandle, connection);

        auto secureFlag = address.secure ? DWORD {WINHTTP_FLAG_SECURE} : DWORD {0};

        auto* request = WinHttpOpenRequest(connection,
                                           L"GET",
                                           address.pathWithQuery.c_str(),
                                           nullptr,
                                           WINHTTP_NO_REFERER,
                                           WINHTTP_DEFAULT_ACCEPT_TYPES,
                                           secureFlag);
        if (!request)
            webSocketThrowLastError("Opening the request");

        adoptHandle(requestHandle, request);

        // Every phase of the handshake is bounded by the connect timeout;
        // once the upgrade is through, a receive waits as long as the peer
        // stays quiet, which for a WebSocket is not an error.
        auto bounded = webSocketBoundedTimeout(options.connectTimeout);
        WinHttpSetTimeouts(request, bounded, bounded, bounded, bounded);

        if (!WinHttpSetOption(
                request, WINHTTP_OPTION_UPGRADE_TO_WEB_SOCKET, nullptr, 0))
            webSocketThrowLastError("Asking for the WebSocket upgrade");

        auto headers = webSocketRequestHeaders(options);

        if (!headers.empty())
            WinHttpAddRequestHeaders(request,
                                     headers.c_str(),
                                     static_cast<DWORD>(headers.size()),
                                     WINHTTP_ADDREQ_FLAG_ADD);

        throwIfAbandoned();

        if (!WinHttpSendRequest(request,
                                WINHTTP_NO_ADDITIONAL_HEADERS,
                                0,
                                WINHTTP_NO_REQUEST_DATA,
                                0,
                                0,
                                0))
            webSocketThrowLastError("Sending the WebSocket handshake");

        if (!WinHttpReceiveResponse(request, nullptr))
            webSocketThrowLastError("Receiving the WebSocket handshake");

        throwIfAbandoned();

        auto status = webSocketQueryStatusCode(request);

        if (status != webSocketSwitchingProtocols)
            throw std::runtime_error("handshake rejected: "
                                     + std::to_string(status));

        auto protocol = webSocketQueryHeader(request, L"Sec-WebSocket-Protocol");

        // The socket handle takes the request's options as it is made, so the
        // infinite receive timeout has to be in place before the upgrade
        // completes; setting it again afterwards costs nothing and covers a
        // WinHTTP that does not pass it on.
        WinHttpSetTimeouts(request, 0, 0, bounded, 0);

        auto* socket = WinHttpWebSocketCompleteUpgrade(request, 0);

        if (!socket)
            webSocketThrowLastError("Completing the WebSocket upgrade");

        adoptHandle(webSocketHandle, socket);
        closeHandle(requestHandle);

        WinHttpSetTimeouts(socket, 0, 0, bounded, 0);

        auto keepAlive = webSocketKeepAliveMilliseconds;
        WinHttpSetOption(socket,
                         WINHTTP_OPTION_WEB_SOCKET_KEEPALIVE_INTERVAL,
                         &keepAlive,
                         static_cast<DWORD>(sizeof(keepAlive)));

        throwIfAbandoned();
        reportOpened(protocol);
    }

    void receiveLoop()
    {
        auto buffer = std::string(webSocketReceiveChunkSize, '\0');
        auto payload = std::string();

        while (!tearingDown.load())
        {
            auto* socket = handleOf(webSocketHandle);

            if (!socket)
                return;

            auto read = DWORD {0};
            auto type = WINHTTP_WEB_SOCKET_BUFFER_TYPE {};

            auto result = WinHttpWebSocketReceive(socket,
                                                  buffer.data(),
                                                  static_cast<DWORD>(buffer.size()),
                                                  &read,
                                                  &type);

            // Only reachable where the handle refused an infinite receive
            // timeout: nothing arrived, which is not a failure.
            if (result == ERROR_WINHTTP_TIMEOUT)
                continue;

            if (result != ERROR_SUCCESS)
            {
                reportReceiveFailure(result);
                return;
            }

            if (type == WINHTTP_WEB_SOCKET_CLOSE_BUFFER_TYPE)
            {
                reportPeerClose(socket);
                return;
            }

            if (payload.size() + read > options.maxMessageSize)
            {
                reportOversizedMessage(socket);
                return;
            }

            payload.append(buffer.data(), read);

            if (webSocketBufferCompletesMessage(type))
            {
                auto kind = webSocketBufferIsText(type) ? MessageType::text
                                                        : MessageType::binary;

                reportReceived(Message {std::move(payload), kind});
                payload.clear();
            }
        }
    }

    void reportPeerClose(HINTERNET socket)
    {
        auto status = webSocketEmptyClose;
        char reason[webSocketMaxCloseReasonLength] {};
        auto consumed = DWORD {0};

        auto queried = WinHttpWebSocketQueryCloseStatus(
            socket, &status, reason, static_cast<DWORD>(sizeof(reason)), &consumed);

        auto text = std::string();

        if (queried == ERROR_SUCCESS && consumed <= sizeof(reason))
            text.assign(reason, consumed);
        else
            status = webSocketEmptyClose;

        // A close we did not ask for is answered here; one we did is the
        // peer's reply to a frame already sent.
        if (!closeRequested.load() && !tearingDown.load())
            shutdown(socket, webSocketEchoableStatus(status), text);

        reportClosed(static_cast<int>(status), text);
    }

    void reportOversizedMessage(HINTERNET socket)
    {
        shutdown(socket, webSocketMessageTooBigClose, {});
        reportFailed("message exceeds maxMessageSize");
    }

    void reportReceiveFailure(DWORD result)
    {
        if (tearingDown.load())
            return;

        if (closeRequested.load())
        {
            reportClosed(webSocketAbnormalClose, {});
            return;
        }

        // What our own teardown leaves behind, arriving before the flag was
        // there to be read: nothing the page hears.
        if (result == ERROR_WINHTTP_OPERATION_CANCELLED
            || result == ERROR_INVALID_HANDLE)
            return;

        reportFailed(webSocketErrorMessage(result, "Receiving the message"));
    }

    void reportHandshakeFailure(const std::string& error)
    {
        if (tearingDown.load())
            return;

        if (closeRequested.load())
        {
            reportClosed(webSocketAbnormalClose, {});
            return;
        }

        reportFailed(error);
    }

    void shutdown(HINTERNET socket, USHORT status, const std::string& reason)
    {
        auto text = reason.substr(0, webSocketMaxCloseReasonLength);
        auto length = static_cast<DWORD>(text.size());

        auto lock = std::scoped_lock(sendMutex);
        WinHttpWebSocketShutdown(
            socket, status, length > 0 ? text.data() : nullptr, length);
    }

    void throwIfAbandoned()
    {
        if (tearingDown.load() || closeRequested.load())
            throw std::runtime_error("The connection was closed while connecting");
    }

    void reportOpened(const std::string& protocol)
    {
        if (terminated.load() || tearingDown.load())
            return;

        sink->opened(protocol);
    }

    void reportReceived(Message message)
    {
        if (terminated.load() || tearingDown.load())
            return;

        sink->received(std::move(message));
    }

    void reportClosed(int code, const std::string& reason)
    {
        if (tearingDown.load() || terminated.exchange(true))
            return;

        sink->closed(code, reason);
    }

    void reportFailed(const std::string& error)
    {
        if (tearingDown.load() || terminated.exchange(true))
            return;

        sink->failed(error);
    }

    HINTERNET handleOf(HINTERNET& slot)
    {
        auto lock = std::scoped_lock(handleMutex);
        return slot;
    }

    void adoptHandle(HINTERNET& slot, HINTERNET handle)
    {
        auto lock = std::scoped_lock(handleMutex);
        slot = handle;
    }

    void closeHandle(HINTERNET& slot)
    {
        auto lock = std::scoped_lock(handleMutex);

        if (slot)
        {
            WinHttpCloseHandle(slot);
            slot = nullptr;
        }
    }

    void closeEveryHandle()
    {
        closeHandle(webSocketHandle);
        closeHandle(requestHandle);
        closeHandle(connectionHandle);
    }

    std::string url;
    Options options;
    std::shared_ptr<Sink> sink;

    std::mutex handleMutex;
    HINTERNET connectionHandle = nullptr;
    HINTERNET requestHandle = nullptr;
    HINTERNET webSocketHandle = nullptr;

    std::mutex sendMutex;

    std::atomic<bool> tearingDown {false};
    std::atomic<bool> closeRequested {false};
    std::atomic<bool> terminated {false};

    std::thread worker;
};

} // namespace

std::unique_ptr<Backend> makeBackend(const std::string& url,
                                     const Options& options,
                                     std::shared_ptr<Sink> sink)
{
    return std::make_unique<WebSocketTransport>(url, options, std::move(sink));
}

bool backendIsSupported()
{
    return true;
}

} // namespace eacp::WebSocket

#include "Backend.h"
#include "Protocol.h"

#include <curl/curl.h>

#include <atomic>
#include <cctype>
#include <cstddef>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>

#if defined(LIBCURL_VERSION_NUM) && LIBCURL_VERSION_NUM >= 0x075600
#define EACP_WEBSOCKET_HAS_CURL_WS 1
#else
#define EACP_WEBSOCKET_HAS_CURL_WS 0
#endif

#if EACP_WEBSOCKET_HAS_CURL_WS
#include <array>
#include <fcntl.h>
#include <poll.h>
#include <unistd.h>
#endif

namespace eacp::WebSocket
{

namespace
{
// What makeBackend hands back where libcurl cannot speak the protocol: the
// Sink marshals the failure, so reporting it from here is a report on the
// caller's thread, which the contract allows.
class WebSocketMissingBackend final : public Backend
{
public:
    explicit WebSocketMissingBackend(const std::shared_ptr<Sink>& sink)
    {
        sink->failed("libcurl has no WebSocket support");
    }

    void send(const Message&) override {}
    void close(int, const std::string&) override {}
};
} // namespace

#if EACP_WEBSOCKET_HAS_CURL_WS

namespace
{
void webSocketInitCurl()
{
    static auto once = []
    {
        curl_global_init(CURL_GLOBAL_DEFAULT);
        return 0;
    }();
    (void) once;
}

char webSocketLower(char c)
{
    return (char) std::tolower((unsigned char) c);
}

bool webSocketStartsWith(std::string_view line, std::string_view lowercaseName)
{
    if (line.size() < lowercaseName.size())
        return false;

    for (auto i = std::size_t {0}; i < lowercaseName.size(); ++i)
        if (webSocketLower(line[i]) != lowercaseName[i])
            return false;

    return true;
}

bool webSocketIsBlank(char c)
{
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

std::string_view webSocketTrim(std::string_view text)
{
    while (!text.empty() && webSocketIsBlank(text.front()))
        text.remove_prefix(1);

    while (!text.empty() && webSocketIsBlank(text.back()))
        text.remove_suffix(1);

    return text;
}

constexpr auto webSocketProtocolHeader =
    std::string_view {"sec-websocket-protocol:"};

size_t webSocketHeaderCallback(char* buffer, size_t size, size_t items, void* userp)
{
    auto total = size * items;
    auto line = std::string_view {buffer, total};
    auto& chosen = *static_cast<std::string*>(userp);

    if (webSocketStartsWith(line, "http/"))
        chosen.clear();
    else if (webSocketStartsWith(line, webSocketProtocolHeader))
        chosen = webSocketTrim(line.substr(webSocketProtocolHeader.size()));

    return total;
}

std::string webSocketProtocolList(const Vector<std::string>& protocols)
{
    auto joined = std::string();

    for (const auto& protocol: protocols)
    {
        if (!joined.empty())
            joined += ", ";

        joined += protocol;
    }

    return joined;
}

struct WebSocketCurlHeaders
{
    WebSocketCurlHeaders() = default;
    WebSocketCurlHeaders(const WebSocketCurlHeaders&) = delete;
    WebSocketCurlHeaders& operator=(const WebSocketCurlHeaders&) = delete;

    ~WebSocketCurlHeaders()
    {
        if (list != nullptr)
            curl_slist_free_all(list);
    }

    void add(const std::string& line)
    {
        list = curl_slist_append(list, line.c_str());
    }

    curl_slist* list = nullptr;
};

// The one fd the message thread can touch: writing a byte drops the worker
// out of poll so a send or a close does not wait out the poll timeout.
class WebSocketWakePipe
{
public:
    WebSocketWakePipe()
    {
        if (::pipe2(fds, O_CLOEXEC | O_NONBLOCK) != 0)
        {
            fds[0] = -1;
            fds[1] = -1;
        }
    }

    WebSocketWakePipe(const WebSocketWakePipe&) = delete;
    WebSocketWakePipe& operator=(const WebSocketWakePipe&) = delete;

    ~WebSocketWakePipe()
    {
        for (auto fd: fds)
            if (fd >= 0)
                ::close(fd);
    }

    void signal() const
    {
        if (fds[1] < 0)
            return;

        auto byte = char {1};
        auto written = ::write(fds[1], &byte, 1);
        (void) written;
    }

    void drain() const
    {
        if (fds[0] < 0)
            return;

        char scratch[64];

        while (::read(fds[0], scratch, sizeof(scratch)) > 0)
        {
        }
    }

    int readEnd() const { return fds[0]; }

private:
    int fds[2] {-1, -1};
};

struct WebSocketOutbound
{
    std::string payload;
    unsigned int flags = CURLWS_TEXT;
    std::size_t offset = 0;
};

// One worker thread owns the easy handle for the connection's whole life: a
// curl easy handle is not thread safe, so send and close only queue and wake,
// and every curl call happens on the worker.
class WebSocketCurlBackend final : public Backend
{
public:
    WebSocketCurlBackend(const std::string& urlToUse,
                         const Options& options,
                         std::shared_ptr<Sink> sinkToUse)
        : sink(std::move(sinkToUse))
        , url(urlToUse)
        , maxMessageSize(options.maxMessageSize)
    {
        webSocketInitCurl();
        handle = curl_easy_init();
        multi = curl_multi_init();

        if (handle == nullptr || multi == nullptr)
        {
            reportFailed("Failed to initialise curl");
            return;
        }

        applyOptions(options);
        curl_multi_add_handle(multi, handle);
        worker = std::thread([this] { run(); });
    }

    ~WebSocketCurlBackend() override
    {
        stopRequested.store(true);
        wake.signal();

        if (worker.joinable())
            worker.join();

        if (multi != nullptr)
        {
            if (handle != nullptr)
                curl_multi_remove_handle(multi, handle);

            curl_multi_cleanup(multi);
        }

        if (handle != nullptr)
            curl_easy_cleanup(handle);
    }

    void send(const Message& message) override
    {
        if (closeRequested.load())
            return;

        auto flags = message.type == MessageType::binary
                         ? (unsigned int) CURLWS_BINARY
                         : (unsigned int) CURLWS_TEXT;

        queueOutbound(message.data, flags);
    }

    void close(int code, const std::string& reason) override
    {
        if (closeRequested.exchange(true))
            return;

        queueOutbound(Protocol::encodeClose(code, reason), CURLWS_CLOSE);
    }

private:
    enum class Handshake
    {
        opened,
        aborted,
        failed,
    };

    void applyOptions(const Options& options)
    {
        curl_easy_setopt(handle, CURLOPT_URL, url.c_str());
        curl_easy_setopt(handle, CURLOPT_CONNECT_ONLY, 2L);
        curl_easy_setopt(handle, CURLOPT_NOSIGNAL, 1L);
        curl_easy_setopt(handle, CURLOPT_ERRORBUFFER, errorBuffer);
        curl_easy_setopt(handle, CURLOPT_HEADERFUNCTION, webSocketHeaderCallback);
        curl_easy_setopt(handle, CURLOPT_HEADERDATA, &chosenProtocol);

        if (options.connectTimeout.count > 0)
        {
            auto limit = (long) options.connectTimeout.count;
            curl_easy_setopt(handle, CURLOPT_CONNECTTIMEOUT_MS, limit);
            curl_easy_setopt(handle, CURLOPT_TIMEOUT_MS, limit);
        }

        for (const auto& [name, value]: options.headers)
            requestHeaders.add(name + ": " + value);

        auto protocols = webSocketProtocolList(options.protocols);

        if (!protocols.empty())
            requestHeaders.add("Sec-WebSocket-Protocol: " + protocols);

        if (requestHeaders.list != nullptr)
            curl_easy_setopt(handle, CURLOPT_HTTPHEADER, requestHeaders.list);
    }

    void run()
    {
        if (!performHandshake())
            return;

        sink->opened(chosenProtocol);
        pump();
    }

    bool performHandshake()
    {
        errorBuffer[0] = '\0';

        auto outcome = runHandshake();

        curl_easy_setopt(handle, CURLOPT_TIMEOUT_MS, 0L);

        if (outcome == Handshake::opened)
            return true;

        if (stopRequested.load())
            return false;

        if (closeRequested.load())
            reportClosed(1006, {});
        else
            reportFailed(handshakeError);

        return false;
    }

    // The upgrade goes through the multi interface for one reason: the poll
    // slice is ours and the wake pipe sits in it, so a close or a destructor
    // arriving mid-handshake is heard at once rather than after
    // curl_easy_perform's own second.
    Handshake runHandshake()
    {
        auto running = 1;

        while (!stopRequested.load() && !closeRequested.load())
        {
            auto status = curl_multi_perform(multi, &running);

            if (status != CURLM_OK)
            {
                handshakeError = curl_multi_strerror(status);
                return Handshake::failed;
            }

            if (running == 0)
                return readHandshakeResult();

            pollHandshake();
        }

        return Handshake::aborted;
    }

    Handshake readHandshakeResult()
    {
        auto remaining = 0;
        auto result = CURLE_OK;

        while (auto* message = curl_multi_info_read(multi, &remaining))
            if (message->msg == CURLMSG_DONE)
                result = message->data.result;

        if (result == CURLE_OK)
            return Handshake::opened;

        handshakeError = errorText(result);
        return Handshake::failed;
    }

    void pollHandshake()
    {
        auto extra = curl_waitfd {};
        extra.fd = (curl_socket_t) wake.readEnd();
        extra.events = CURL_WAIT_POLLIN;

        auto count = wake.readEnd() >= 0 ? 1U : 0U;

        curl_multi_poll(multi, &extra, count, handshakePollSliceMs, nullptr);
        wake.drain();
    }

    void pump()
    {
        while (!stopRequested.load())
        {
            if (!pumpOutbound())
                return;

            if (!pumpInbound())
                return;

            if (stopRequested.load())
                return;

            auto events =
                pending.has_value() ? (short) (POLLIN | POLLOUT) : (short) POLLIN;

            waitForTransport(events, pollSliceMs);
        }
    }

    bool pumpOutbound()
    {
        for (;;)
        {
            if (!pending.has_value() && !takeNextOutbound())
                return true;

            auto result = sendPending();

            if (result == CURLE_AGAIN)
                return true;

            if (result != CURLE_OK)
                return reportTransportEnd(result);

            pending.reset();
        }
    }

    bool takeNextOutbound()
    {
        auto lock = std::lock_guard(mutex);

        if (outbound.empty())
            return false;

        pending = std::move(outbound.front());
        outbound.pop_front();
        return true;
    }

    CURLcode sendPending()
    {
        auto& item = *pending;

        for (;;)
        {
            auto sent = std::size_t {0};
            auto flags = item.offset == 0
                             ? item.flags
                             : (unsigned int) (item.flags | CURLWS_OFFSET);

            auto result = curl_ws_send(handle,
                                       item.payload.data() + item.offset,
                                       item.payload.size() - item.offset,
                                       &sent,
                                       0,
                                       flags);

            if (result != CURLE_OK)
                return result;

            item.offset += sent;

            if (item.offset >= item.payload.size())
                return CURLE_OK;
        }
    }

    bool pumpInbound()
    {
        for (;;)
        {
            auto received = std::size_t {0};
            const curl_ws_frame* meta = nullptr;

            auto result = curl_ws_recv(
                handle, receiveBuffer, sizeof(receiveBuffer), &received, &meta);

            if (result == CURLE_AGAIN)
                return true;

            if (result != CURLE_OK || meta == nullptr)
                return reportTransportEnd(result);

            if (!handleFrame(*meta, std::string_view {receiveBuffer, received}))
                return false;
        }
    }

    bool handleFrame(const curl_ws_frame& meta, std::string_view chunk)
    {
        auto flags = (unsigned int) meta.flags;

        if ((flags & CURLWS_CLOSE) != 0)
            return handleClose(meta, chunk);

        if ((flags & CURLWS_PING) != 0)
            return handlePing(meta, chunk);

        if ((flags & CURLWS_PONG) != 0)
        {
            if (meta.bytesleft == 0)
                controlPayload.clear();

            return true;
        }

        return handleData(meta, chunk);
    }

    bool handlePing(const curl_ws_frame& meta, std::string_view chunk)
    {
        controlPayload.append(chunk);

        if (meta.bytesleft > 0)
            return true;

        auto payload = std::move(controlPayload);
        controlPayload.clear();

        sendControlFrame(payload, CURLWS_PONG);
        return true;
    }

    bool handleClose(const curl_ws_frame& meta, std::string_view chunk)
    {
        controlPayload.append(chunk);

        if (meta.bytesleft > 0)
            return true;

        auto payload = std::move(controlPayload);
        controlPayload.clear();

        auto status = Protocol::decodeClose(payload);

        if (!closeRequested.exchange(true))
            sendControlFrame(payload, CURLWS_CLOSE);

        reportClosed(status.code, status.reason);
        return false;
    }

    bool handleData(const curl_ws_frame& meta, std::string_view chunk)
    {
        auto flags = (unsigned int) meta.flags;

        if (!messageInProgress)
        {
            messageInProgress = true;
            messageType = (flags & CURLWS_BINARY) != 0 ? MessageType::binary
                                                       : MessageType::text;
            messagePayload.clear();
        }

        auto left =
            meta.bytesleft > 0 ? (std::size_t) meta.bytesleft : std::size_t {0};

        if (messagePayload.size() + chunk.size() + left > maxMessageSize)
            return rejectOversized();

        messagePayload.append(chunk);

        if ((flags & CURLWS_CONT) != 0 || meta.bytesleft > 0)
            return true;

        auto message = Message {};
        message.data = std::move(messagePayload);
        message.type = messageType;

        messagePayload.clear();
        messageInProgress = false;

        sink->received(std::move(message));
        return true;
    }

    bool rejectOversized()
    {
        closeRequested.store(true);
        sendControlFrame(Protocol::encodeClose(1009, "message too big"),
                         CURLWS_CLOSE);

        reportFailed("message exceeds maxMessageSize");
        return false;
    }

    // A pong or a close reply is worth waiting a moment for, but never worth
    // splicing into a frame the queue has already started sending.
    void sendControlFrame(const std::string& payload, unsigned int flags)
    {
        if (pending.has_value() && pending->offset > 0)
        {
            auto lock = std::lock_guard(mutex);
            outbound.push_front(WebSocketOutbound {payload, flags, 0});
            return;
        }

        auto deadline = Time::Deadline {Time::MS {controlSendTimeoutMs}};
        auto offset = std::size_t {0};

        while (!stopRequested.load())
        {
            auto sent = std::size_t {0};
            auto frameFlags =
                offset == 0 ? flags : (unsigned int) (flags | CURLWS_OFFSET);

            auto result = curl_ws_send(handle,
                                       payload.data() + offset,
                                       payload.size() - offset,
                                       &sent,
                                       0,
                                       frameFlags);

            if (result == CURLE_OK)
            {
                offset += sent;

                if (offset >= payload.size())
                    return;

                continue;
            }

            if (result != CURLE_AGAIN || deadline.expired())
                return;

            waitForTransport(POLLOUT, controlPollSliceMs);
        }
    }

    void queueOutbound(std::string payload, unsigned int flags)
    {
        if (terminalReported.load())
            return;

        {
            auto lock = std::lock_guard(mutex);
            outbound.push_back(WebSocketOutbound {std::move(payload), flags, 0});
        }

        wake.signal();
    }

    void waitForTransport(short events, int timeoutMs)
    {
        auto socket = CURL_SOCKET_BAD;
        curl_easy_getinfo(handle, CURLINFO_ACTIVESOCKET, &socket);

        auto fds = std::array<pollfd, 2> {};
        auto count = nfds_t {0};

        if (socket != CURL_SOCKET_BAD)
        {
            fds[count].fd = (int) socket;
            fds[count].events = events;
            ++count;
        }

        if (wake.readEnd() >= 0)
        {
            fds[count].fd = wake.readEnd();
            fds[count].events = POLLIN;
            ++count;
        }

        if (count == 0)
        {
            Time::sleep(Time::MS {timeoutMs});
            return;
        }

        ::poll(fds.data(), count, timeoutMs);
        wake.drain();
    }

    bool reportTransportEnd(CURLcode result)
    {
        if (closeRequested.load())
            reportClosed(1006, {});
        else
            reportFailed(errorText(result));

        return false;
    }

    std::string errorText(CURLcode result) const
    {
        if (errorBuffer[0] != '\0')
            return errorBuffer;

        return curl_easy_strerror(result);
    }

    void reportClosed(int code, const std::string& reason)
    {
        if (!terminalReported.exchange(true))
            sink->closed(code, reason);
    }

    void reportFailed(const std::string& error)
    {
        if (!terminalReported.exchange(true))
            sink->failed(error);
    }

    static constexpr auto pollSliceMs = 100;
    static constexpr auto handshakePollSliceMs = 100;
    static constexpr auto controlPollSliceMs = 20;
    static constexpr auto controlSendTimeoutMs = 250;

    std::shared_ptr<Sink> sink;
    std::string url;
    std::size_t maxMessageSize = 0;

    CURL* handle = nullptr;
    CURLM* multi = nullptr;
    WebSocketCurlHeaders requestHeaders;
    std::string chosenProtocol;
    std::string handshakeError;
    char errorBuffer[CURL_ERROR_SIZE] {};

    WebSocketWakePipe wake;
    std::mutex mutex;
    std::deque<WebSocketOutbound> outbound;

    std::atomic<bool> stopRequested {false};
    std::atomic<bool> closeRequested {false};
    std::atomic<bool> terminalReported {false};

    std::optional<WebSocketOutbound> pending;
    char receiveBuffer[16384] {};
    std::string controlPayload;
    std::string messagePayload;
    MessageType messageType = MessageType::text;
    bool messageInProgress = false;

    std::thread worker;
};
} // namespace

bool backendIsSupported()
{
    webSocketInitCurl();

    const auto* info = curl_version_info(CURLVERSION_NOW);

    if (info == nullptr || info->protocols == nullptr)
        return false;

    for (auto* entry = info->protocols; *entry != nullptr; ++entry)
        if (std::string_view {*entry} == "ws")
            return true;

    return false;
}

std::unique_ptr<Backend> makeBackend(const std::string& url,
                                     const Options& options,
                                     std::shared_ptr<Sink> sink)
{
    if (backendIsSupported())
        return std::make_unique<WebSocketCurlBackend>(url, options, std::move(sink));

    return std::make_unique<WebSocketMissingBackend>(sink);
}

#else

bool backendIsSupported()
{
    return false;
}

std::unique_ptr<Backend>
    makeBackend(const std::string&, const Options&, std::shared_ptr<Sink> sink)
{
    return std::make_unique<WebSocketMissingBackend>(sink);
}

#endif

} // namespace eacp::WebSocket

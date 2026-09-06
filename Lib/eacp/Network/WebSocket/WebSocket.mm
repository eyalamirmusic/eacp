#import <Foundation/Foundation.h>
#import <Network/Network.h>

#include "Backend.h"
#include <eacp/Core/ObjC/AutoReleasePool.h>
#include <eacp/Core/ObjC/Strings.h>
#include <eacp/Core/Utils/Strings.h>

#include <memory>
#include <mutex>
#include <stdexcept>
#include <string_view>

// Network.framework's WebSocket, which frames, masks and answers pings
// itself, rather than NSURLSessionWebSocketTask: that one's
// cancelWithCloseCode: tears the transport down before its close frame is on
// the wire - a few percent of the time on a desktop, every time on GitHub's
// macOS runners. Here the close frame is a send like any other, and the
// connection is not cancelled until the peer has answered it or the stream
// has ended. Every file-scope name is prefixed, the library being one
// translation unit under a unity build.
namespace eacp::WebSocket
{
namespace
{

constexpr auto webSocketMaxCloseReasonLength = std::size_t {123};

// A close frame Network.framework has no receive pending for is not held
// back: it ends the connection as ENOTCONN instead, and its code and reason
// are gone. So one receive covers the frame being delivered and the other
// whatever the peer put behind it in the same read - a last message and a
// goodbye being the pair that arrives that way.
constexpr auto webSocketReceivesInFlight = 2;
constexpr auto webSocketNormalClose = 1000;
constexpr auto webSocketEmptyClose = 1005;
constexpr auto webSocketAbnormalClose = 1006;

// RFC 6455's close codes 1005 and 1006 are the two a peer may never put on
// the wire, so an echo of either goes out as a plain 1000.
int webSocketEchoableCode(int code)
{
    if (code == webSocketEmptyClose || code == webSocketAbnormalClose)
        return webSocketNormalClose;

    return code;
}

// §5.5: a close reason is what is left of a control frame's 125 bytes once
// the code has taken two.
std::string webSocketTrimReason(const std::string& reason)
{
    if (reason.size() <= webSocketMaxCloseReasonLength)
        return reason;

    return reason.substr(0, webSocketMaxCloseReasonLength);
}

bool webSocketIsSecureUrl(const std::string& url)
{
    constexpr auto scheme = std::string_view {"wss://"};

    if (url.size() < scheme.size())
        return false;

    return Strings::equalsCaseInsensitive(
        std::string_view {url}.substr(0, scheme.size()), scheme);
}

// §5.6 has no frame for a text payload that is not UTF-8.
bool webSocketIsUtf8(const std::string& text)
{
    auto* string = [[NSString alloc] initWithBytes:text.data()
                                            length:text.size()
                                          encoding:NSUTF8StringEncoding];
    auto valid = string != nil;
    [string release];
    return valid;
}

std::string webSocketErrorText(nw_error_t error)
{
    if (error == nullptr)
        return {};

    auto cfError = nw_error_copy_cf_error(error);
    auto text = Strings::toStdString((__bridge NSError*) cfError);
    CFRelease(cfError);
    return text;
}

std::string webSocketBytesOf(dispatch_data_t content)
{
    __block auto bytes = std::string();

    if (content == nullptr)
        return bytes;

    dispatch_data_apply(content,
                        ^(dispatch_data_t, size_t, const void* buffer, size_t size) {
                          bytes.append((const char*) buffer, size);
                          return true;
                        });

    return bytes;
}

// Retained, or null for nothing at all. dispatch_data_create copies the
// bytes, so the string need not outlive the send.
dispatch_data_t webSocketDataOf(const std::string& bytes)
{
    if (bytes.empty())
        return nullptr;

    return dispatch_data_create(
        bytes.data(), bytes.size(), nullptr, DISPATCH_DATA_DESTRUCTOR_DEFAULT);
}

nw_endpoint_t webSocketEndpointFor(const std::string& url)
{
    if (url.empty())
        throw std::invalid_argument("URL cannot be empty");

    auto endpoint = nw_endpoint_create_url(url.c_str());

    if (endpoint == nullptr)
        throw std::runtime_error("Malformed URL format");

    return endpoint;
}

uint32_t webSocketWholeSeconds(Time::MS timeout)
{
    return (uint32_t) ((timeout.count + 999) / 1000);
}

// TLS for wss and none for ws. The connect timeout bounds the TCP handshake
// here; the WebSocket one it bounds from a timer, Network.framework having
// no bound of its own on the upgrade.
nw_parameters_t webSocketParametersFor(const std::string& url,
                                       const Options& options)
{
    auto seconds = webSocketWholeSeconds(options.connectTimeout);

    auto configureTcp = ^(nw_protocol_options_t tcp) {
      if (seconds > 0)
          nw_tcp_options_set_connection_timeout(tcp, seconds);
    };

    auto configureTls = webSocketIsSecureUrl(url) ? NW_PARAMETERS_DEFAULT_CONFIGURATION
                                                  : NW_PARAMETERS_DISABLE_PROTOCOL;

    auto parameters = nw_parameters_create_secure_tcp(configureTls, configureTcp);
    auto stack = nw_parameters_copy_default_protocol_stack(parameters);
    auto webSocket = nw_ws_create_options(nw_ws_version_13);

    nw_ws_options_set_auto_reply_ping(webSocket, true);
    nw_ws_options_set_maximum_message_size(webSocket, options.maxMessageSize);

    for (const auto& [name, value]: options.headers)
        nw_ws_options_add_additional_header(webSocket, name.c_str(), value.c_str());

    for (const auto& protocol: options.protocols)
        nw_ws_options_add_subprotocol(webSocket, protocol.c_str());

    nw_protocol_stack_prepend_application_protocol(stack, webSocket);

    nw_release(webSocket);
    nw_release(stack);
    return parameters;
}

// What the connection's handlers and the backend share. The handlers outlive
// the backend - a cancelled connection reports on its queue after the
// Connection has gone - so everything they touch lives here behind a lock,
// held by shared_ptr from every block. It is also the gate that keeps the
// Sink's contract: one terminal report and nothing after it, however many
// ways the connection has of ending.
//
// Every Network.framework call made under the lock is asynchronous, its
// handlers always dispatched to the queue rather than run inline, which is
// what makes holding the lock across them safe.
class WebSocketContext : public std::enable_shared_from_this<WebSocketContext>
{
public:
    WebSocketContext(std::shared_ptr<Sink> sinkToUse, Time::MS connectTimeoutToUse)
        : sink(std::move(sinkToUse))
        , connectTimeout(connectTimeoutToUse)
        , queue(dispatch_queue_create("eacp.websocket", DISPATCH_QUEUE_SERIAL))
        , webSocketDefinition(nw_protocol_copy_ws_definition())
    {
    }

    ~WebSocketContext()
    {
        nw_release(webSocketDefinition);
        dispatch_release(queue);
    }

    // Takes the connection, retained, and starts it.
    void start(nw_connection_t connectionToUse)
    {
        auto lock = std::scoped_lock(mutex);
        auto self = shared_from_this();

        connection = connectionToUse;

        nw_connection_set_queue(connection, queue);
        nw_connection_set_state_changed_handler(
            connection, ^(nw_connection_state_t state, nw_error_t error) {
              self->stateChanged(state, error);
            });
        nw_connection_start(connection);

        for (auto pending = 0; pending < webSocketReceivesInFlight; ++pending)
            armReceive();

        scheduleConnectTimeout();
    }

    void send(const Message& message)
    {
        auto lock = std::scoped_lock(mutex);

        if (finished || !opened || closeSent)
            return;

        if (message.type == MessageType::text && !webSocketIsUtf8(message.data))
        {
            streamEnded("Text message is not valid UTF-8");
            return;
        }

        auto opcode = message.type == MessageType::text ? nw_ws_opcode_text
                                                        : nw_ws_opcode_binary;

        auto metadata = nw_ws_create_metadata(opcode);
        sendFrame(metadata, message.data);
        nw_release(metadata);
    }

    void close(int code, const std::string& reason)
    {
        auto lock = std::scoped_lock(mutex);

        if (finished || closeSent)
            return;

        // Before open there is no handshake to close: the attempt is
        // abandoned, and reads as the abnormal closure it is.
        if (!opened)
        {
            reportClosed(webSocketAbnormalClose, {});
            cancel();
            return;
        }

        sendClose(code, webSocketTrimReason(reason));
    }

    // The backend is going away: nothing the connection says from here on is
    // the Sink's business, and the transport goes without waiting on the
    // network.
    void detach()
    {
        auto lock = std::scoped_lock(mutex);

        finished = true;
        cancel();

        if (connection != nullptr)
        {
            nw_release(connection);
            connection = nullptr;
        }
    }

private:
    void stateChanged(nw_connection_state_t state, nw_error_t error)
    {
        auto pool = ObjC::AutoReleasePool();
        auto lock = std::scoped_lock(mutex);

        if (finished)
            return;

        switch (state)
        {
            case nw_connection_state_ready:
                becomeReady();
                return;

            // A refusal arrives as waiting, the connection meaning to try
            // again once a path appears; nobody here is owed that wait.
            case nw_connection_state_waiting:
                if (error != nullptr)
                    streamEnded(webSocketErrorText(error));
                return;

            case nw_connection_state_failed:
            case nw_connection_state_cancelled:
                streamEnded(webSocketErrorText(error));
                return;

            default:
                return;
        }
    }

    void received(dispatch_data_t content,
                  nw_content_context_t context,
                  bool isComplete,
                  nw_error_t error)
    {
        auto pool = ObjC::AutoReleasePool();
        auto lock = std::scoped_lock(mutex);

        if (finished)
            return;

        if (error != nullptr)
        {
            streamEnded(webSocketErrorText(error));
            return;
        }

        auto metadata = context != nullptr
                            ? nw_content_context_copy_protocol_metadata(
                                context, webSocketDefinition)
                            : nullptr;

        // No frame in it: the stream has ended.
        if (metadata == nullptr)
        {
            streamEnded({});
            return;
        }

        auto opcode = nw_ws_metadata_get_opcode(metadata);
        auto closeCode = (int) nw_ws_metadata_get_close_code(metadata);
        nw_release(metadata);

        // Every path that is not the end of the connection arms the next
        // receive before it hands this frame on, so the framework never holds
        // one with nobody to give it to.
        switch (opcode)
        {
            case nw_ws_opcode_close:
                peerClosed(closeCode, webSocketBytesOf(content));
                return;

            case nw_ws_opcode_text:
            case nw_ws_opcode_binary:
            case nw_ws_opcode_cont:
                armReceive();
                collect(opcode, webSocketBytesOf(content), isComplete);
                return;

            // Pings are answered by the framework, pongs are nobody's business
            case nw_ws_opcode_ping:
            case nw_ws_opcode_pong:
                armReceive();
                return;

            default:
                streamEnded({});
                return;
        }
    }

    void connectTimedOut()
    {
        auto lock = std::scoped_lock(mutex);

        if (finished || opened)
            return;

        streamEnded("Connecting timed out");
    }

    void sendFailed(const std::string& error)
    {
        auto lock = std::scoped_lock(mutex);

        if (finished)
            return;

        streamEnded(error);
    }

    void closeAnswered()
    {
        auto lock = std::scoped_lock(mutex);
        cancel();
    }

    void becomeReady()
    {
        if (opened)
            return;

        auto response = serverResponse();

        if (response != nullptr
            && nw_ws_response_get_status(response) == nw_ws_response_status_reject)
        {
            nw_release(response);
            streamEnded("The server rejected the WebSocket handshake");
            return;
        }

        auto* chosen = response != nullptr
                           ? nw_ws_response_get_selected_subprotocol(response)
                           : nullptr;

        auto protocol = chosen != nullptr ? std::string(chosen) : std::string();

        if (response != nullptr)
            nw_release(response);

        opened = true;
        sink->opened(protocol);
    }

    nw_ws_response_t serverResponse()
    {
        auto metadata =
            nw_connection_copy_protocol_metadata(connection, webSocketDefinition);

        if (metadata == nullptr)
            return nullptr;

        auto response = nw_ws_metadata_copy_server_response(metadata);
        nw_release(metadata);
        return response;
    }

    // Armed from the start rather than from the handshake, and replaced the
    // moment one is spent, so a delivery is always waiting: a peer that
    // closes as soon as it has answered the upgrade used to find nothing
    // there. An error ends the loop and the connection with it.
    void armReceive()
    {
        auto self = shared_from_this();

        nw_connection_receive_message(
            connection,
            ^(dispatch_data_t content,
              nw_content_context_t context,
              bool isComplete,
              nw_error_t error) {
              self->received(content, context, isComplete, error);
            });
    }

    // Whole messages as a rule, but a framework that hands a message over in
    // pieces is joined back together here rather than trusted not to.
    void collect(nw_ws_opcode_t opcode, std::string bytes, bool isComplete)
    {
        if (!assembling)
        {
            assembling = true;
            assembly.clear();
            assemblyType = opcode == nw_ws_opcode_binary ? MessageType::binary
                                                         : MessageType::text;
        }

        assembly += bytes;

        if (!isComplete)
            return;

        assembling = false;
        sink->received({std::move(assembly), assemblyType});
        assembly.clear();
    }

    // §5.5.1: a close we did not ask for is answered with its own code, and
    // the connection let go once the answer is out; one we did ask for is the
    // peer's reply to a frame already sent.
    void peerClosed(int code, const std::string& reason)
    {
        auto status = code > 0 ? code : webSocketEmptyClose;

        if (closeSent)
        {
            reportClosed(status, reason);
            cancel();
            return;
        }

        auto self = shared_from_this();

        sendClose(webSocketEchoableCode(status), reason, ^(nw_error_t) {
          self->closeAnswered();
        });

        reportClosed(status, reason);
    }

    // The transport is over, with or without a word from the peer: after a
    // close of ours that is the abnormal closure §7.1.5 describes, before
    // one it is a failure.
    void streamEnded(const std::string& error)
    {
        if (closeSent)
            reportClosed(webSocketAbnormalClose, {});
        else if (!error.empty())
            reportFailed(error);
        else
            reportFailed("The connection ended without a close frame");

        cancel();
    }

    void sendClose(int code,
                   const std::string& reason,
                   nw_connection_send_completion_t completion = nullptr)
    {
        closeSent = true;

        auto metadata = nw_ws_create_metadata(nw_ws_opcode_close);
        nw_ws_metadata_set_close_code(metadata, (nw_ws_close_code_t) code);
        sendFrame(metadata, reason, completion);
        nw_release(metadata);
    }

    void sendFrame(nw_protocol_metadata_t metadata,
                   const std::string& bytes,
                   nw_connection_send_completion_t completion = nullptr)
    {
        auto self = shared_from_this();

        auto context = nw_content_context_create("eacp.websocket.frame");
        nw_content_context_set_metadata_for_protocol(context, metadata);

        auto data = webSocketDataOf(bytes);

        nw_connection_send(connection, data, context, true, ^(nw_error_t error) {
          if (error != nullptr)
              self->sendFailed(webSocketErrorText(error));

          if (completion != nullptr)
              completion(error);
        });

        if (data != nullptr)
            dispatch_release(data);

        nw_release(context);
    }

    void scheduleConnectTimeout()
    {
        if (connectTimeout.count <= 0)
            return;

        auto weak = std::weak_ptr<WebSocketContext>(shared_from_this());
        auto delay = dispatch_time(DISPATCH_TIME_NOW,
                                   (int64_t) connectTimeout.count * NSEC_PER_MSEC);

        dispatch_after(delay, queue, ^{
          if (auto self = weak.lock())
              self->connectTimedOut();
        });
    }

    void cancel()
    {
        if (connection == nullptr || cancelled)
            return;

        cancelled = true;
        nw_connection_cancel(connection);
    }

    void reportClosed(int code, const std::string& reason)
    {
        finished = true;
        sink->closed(code, reason);
    }

    void reportFailed(const std::string& error)
    {
        finished = true;
        sink->failed(error);
    }

    std::mutex mutex;
    std::shared_ptr<Sink> sink;
    Time::MS connectTimeout;
    dispatch_queue_t queue;
    nw_protocol_definition_t webSocketDefinition;
    nw_connection_t connection = nullptr;

    bool opened = false;
    bool finished = false;
    bool closeSent = false;
    bool cancelled = false;

    bool assembling = false;
    std::string assembly;
    MessageType assemblyType = MessageType::text;
};

class WebSocketNetworkBackend final : public Backend
{
public:
    WebSocketNetworkBackend(const std::string& url,
                            const Options& options,
                            std::shared_ptr<Sink> sink)
        : context(std::make_shared<WebSocketContext>(std::move(sink),
                                                     options.connectTimeout))
    {
        auto pool = ObjC::AutoReleasePool();

        auto endpoint = webSocketEndpointFor(url);
        auto parameters = webSocketParametersFor(url, options);
        auto connection = nw_connection_create(endpoint, parameters);

        nw_release(parameters);
        nw_release(endpoint);

        if (connection == nullptr)
            throw std::runtime_error("Could not create the WebSocket connection");

        context->start(connection);
    }

    ~WebSocketNetworkBackend() override
    {
        auto pool = ObjC::AutoReleasePool();
        context->detach();
    }

    void send(const Message& message) override
    {
        auto pool = ObjC::AutoReleasePool();
        context->send(message);
    }

    void close(int code, const std::string& reason) override
    {
        auto pool = ObjC::AutoReleasePool();
        context->close(code, reason);
    }

private:
    std::shared_ptr<WebSocketContext> context;
};

} // namespace

std::unique_ptr<Backend> makeBackend(const std::string& url,
                                     const Options& options,
                                     std::shared_ptr<Sink> sink)
{
    return std::make_unique<WebSocketNetworkBackend>(url, options, std::move(sink));
}

bool backendIsSupported()
{
    return true;
}

} // namespace eacp::WebSocket

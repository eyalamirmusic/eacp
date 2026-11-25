#include "Backend.h"

#include <emscripten/emscripten.h>

#include <map>
#include <stdexcept>

// The browser's own WebSocket, keyed by an id shared with WebSocket-Web.js,
// does the handshake, framing, pings and closing handshake. What it cannot do -
// caller headers, close codes outside 1000 and 3000-4999, a message size
// bound - is decided here.

extern "C"
{
    void eacp_web_socket_open(int id,
                              const char* url,
                              const char* protocols,
                              double connectTimeoutMs);
    void eacp_web_socket_send(int id, const void* data, int length, int binary);
    void eacp_web_socket_close(int id, int code, const char* reason);
    void eacp_web_socket_release(int id);
}

namespace eacp::WebSocket
{
namespace
{
struct WebSocketWebState
{
    std::shared_ptr<Sink> sink;
    int maxMessageSize = 0;
    bool ended = false;
};

std::map<int, WebSocketWebState>& webSocketWebStates()
{
    static auto states = std::map<int, WebSocketWebState> {};
    return states;
}

WebSocketWebState* findWebSocketWebState(int id)
{
    auto& states = webSocketWebStates();
    auto found = states.find(id);
    return found == states.end() ? nullptr : &found->second;
}

// The terminal report: once, and the browser's socket is let go with it.
void failWebSocketWeb(int id, WebSocketWebState& state, const std::string& error)
{
    if (std::exchange(state.ended, true))
        return;

    eacp_web_socket_release(id);
    state.sink->failed(error);
}

std::string webSocketWebProtocolList(const Vector<std::string>& protocols)
{
    auto list = std::string {};

    for (const auto& protocol: protocols)
        list += protocol + "\n";

    return list;
}

// A browser refuses any other code from a script; 1000 is the one that means
// what every code a caller sends means: this side is done.
int webSocketWebCloseCode(int code)
{
    return code == 1000 || (code >= 3000 && code <= 4999) ? code : 1000;
}

class WebSocketWebBackend final : public Backend
{
public:
    WebSocketWebBackend(const std::string& url,
                        const Options& options,
                        std::shared_ptr<Sink> sink)
        : id(nextId++)
    {
        webSocketWebStates()[id] = {std::move(sink), options.maxMessageSize};

        eacp_web_socket_open(id,
                             url.c_str(),
                             webSocketWebProtocolList(options.protocols).c_str(),
                             (double) options.connectTimeout.count);
    }

    ~WebSocketWebBackend() override
    {
        eacp_web_socket_release(id);
        webSocketWebStates().erase(id);
    }

    WebSocketWebBackend(const WebSocketWebBackend&) = delete;
    WebSocketWebBackend& operator=(const WebSocketWebBackend&) = delete;

    void send(const Message& message) override
    {
        eacp_web_socket_send(id,
                             message.data.data(),
                             (int) message.data.size(),
                             message.type == MessageType::binary ? 1 : 0);
    }

    void close(int code, const std::string& reason) override
    {
        eacp_web_socket_close(id, webSocketWebCloseCode(code), reason.c_str());
    }

private:
    static inline int nextId = 1;
    int id;
};
} // namespace

bool backendIsSupported()
{
    return true;
}

std::unique_ptr<Backend> makeBackend(const std::string& url,
                                     const Options& options,
                                     std::shared_ptr<Sink> sink)
{
    if (!options.headers.empty())
        throw std::runtime_error {
            "A browser's WebSocket cannot send headers of the caller's"};

    return std::make_unique<WebSocketWebBackend>(url, options, std::move(sink));
}
} // namespace eacp::WebSocket

extern "C"
{
    EMSCRIPTEN_KEEPALIVE void eacp_web_socket_opened(int id, const char* protocol)
    {
        using namespace eacp::WebSocket;
        auto* state = findWebSocketWebState(id);

        if (state == nullptr || state->ended)
            return;

        state->sink->opened(protocol);
    }

    EMSCRIPTEN_KEEPALIVE void
        eacp_web_socket_message(int id, const char* data, int length, int binary)
    {
        using namespace eacp::WebSocket;
        auto* state = findWebSocketWebState(id);

        if (state == nullptr || state->ended)
            return;

        if (length > state->maxMessageSize)
            return failWebSocketWeb(id, *state, "message exceeds maxMessageSize");

        auto message = Message {};
        message.data.assign(data, (std::size_t) length);
        message.type = binary != 0 ? MessageType::binary : MessageType::text;
        state->sink->received(std::move(message));
    }

    EMSCRIPTEN_KEEPALIVE void
        eacp_web_socket_closed(int id, int code, const char* reason)
    {
        using namespace eacp::WebSocket;
        auto* state = findWebSocketWebState(id);

        if (state == nullptr || std::exchange(state->ended, true))
            return;

        state->sink->closed(code, reason);
    }

    EMSCRIPTEN_KEEPALIVE void eacp_web_socket_failed(int id, const char* error)
    {
        using namespace eacp::WebSocket;

        if (auto* state = findWebSocketWebState(id))
            failWebSocketWeb(id, *state, error);
    }
}

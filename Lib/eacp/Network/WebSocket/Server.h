#pragma once

#include "WebSocket.h"

#include <cstdint>

namespace eacp::WebSocket
{

using ClientId = std::uint64_t;

// What a client asked for in its upgrade request
struct Handshake
{
    std::string path;
    std::map<std::string, std::string> headers;
    std::string protocol;
    std::string peerHost;
};

// protocols is what the server will agree to: the first one a client offers
// that is in the list is selected, none when the list is empty.
struct ServerOptions
{
    BindInterface bindTo = BindInterface::loopback;
    Vector<std::string> protocols;
    std::size_t maxMessageSize = 64 * 1024 * 1024;
    Time::MS closeTimeout {5000};
};

// Every callback runs on the message thread, none inside a Server call and
// none once the Server is destroyed. A client's id is good from its onConnect
// to its onDisconnect; a transport that ends without a close frame
// disconnects with 1006. onError is the listener failing: the server accepts
// nobody after it, the clients it has are unaffected.
struct ServerCallbacks
{
    std::function<void(ClientId, const Handshake&)> onConnect =
        [](ClientId, const Handshake&) {};
    std::function<void(ClientId, const Message&)> onMessage =
        [](ClientId, const Message&) {};
    std::function<void(ClientId, const CloseStatus&)> onDisconnect =
        [](ClientId, const CloseStatus&) {};
    std::function<void(const std::string& error)> onError =
        [](const std::string&) {};
};

// An RFC 6455 server over TCP::Listener, speaking Protocol.h: an accept
// thread and one thread per client. Used from the message thread only;
// stop() and the destructor close every client with 1001 and join.
class Server
{
public:
    Server(ServerCallbacks callbacks, ServerOptions options = {});
    ~Server();

    Server(const Server&) = delete;
    Server& operator=(const Server&) = delete;

    // Port 0 asks for an ephemeral one; false when the port cannot be bound
    bool listen(std::uint16_t port);
    void stop();

    [[nodiscard]] bool isListening() const;
    [[nodiscard]] std::uint16_t boundPort() const;
    [[nodiscard]] std::size_t clientCount() const;

    // Dropped for an id that is not connected
    void send(ClientId client, std::string_view text);
    void sendBinary(ClientId client, std::string_view bytes);
    void broadcast(std::string_view text);
    void close(ClientId client, int code = 1000, std::string_view reason = {});

private:
    struct Impl;
    OwningPointer<Impl> impl;
};

} // namespace eacp::WebSocket

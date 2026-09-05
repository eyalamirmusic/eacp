#include "Server.h"

#include "../TCP/Listener.h"
#include "Protocol.h"

#include <eacp/Core/Threads/EventLoop.h>
#include <eacp/Core/Threads/ThreadUtils.h>

#include <atomic>
#include <mutex>
#include <thread>
#include <vector>

namespace eacp::WebSocket
{
namespace
{
// Every blocking call the server makes wakes this often, so a thread told to
// stop notices within one slice instead of at the end of a timeout nobody is
// waiting out. It is what bounds stop() and the destructor.
constexpr auto webSocketServerSlice = Time::MS {200};

// How long a peer that has connected and then not finished its upgrade
// request may hold a thread of its own.
constexpr auto webSocketServerHandshakeTimeout = Time::MS {10000};

constexpr auto webSocketServerReadChunk = std::size_t {65536};
constexpr auto webSocketServerMaxRequestLines = 100;

// §5.5's cap on a control frame's payload, less the two bytes of the code.
constexpr auto webSocketServerMaxCloseReason = std::size_t {123};

// A frame header at its longest - two bytes, eight of length, four of mask -
// which is the slack a limit on the message has to leave the framing itself.
constexpr auto webSocketServerFrameHeader = std::size_t {14};

TCP::Timeouts webSocketServerTimeouts()
{
    return {webSocketServerSlice, webSocketServerSlice};
}

std::string webSocketServerBadRequest()
{
    return "HTTP/1.1 400 Bad Request\r\nContent-Length: 0\r\n"
           "Connection: close\r\n\r\n";
}

std::vector<std::string> webSocketServerCommaList(std::string_view text)
{
    auto items = std::vector<std::string>();

    while (!text.empty())
    {
        auto comma = text.find(',');
        auto piece = Strings::trim(text.substr(0, comma));

        if (!piece.empty())
            items.push_back(std::move(piece));

        if (comma == std::string_view::npos)
            break;

        text.remove_prefix(comma + 1);
    }

    return items;
}

// What the server agrees to: the first protocol the client offered that the
// options name, and none at all where the options name none.
std::string webSocketServerChosenProtocol(const Vector<std::string>& supported,
                                          std::string_view offered)
{
    for (const auto& offer: webSocketServerCommaList(offered))
        if (supported.contains(offer))
            return offer;

    return {};
}

std::string webSocketServerHandshakeResponse(const std::string& key,
                                             const std::string& protocol)
{
    auto response = std::string("HTTP/1.1 101 Switching Protocols\r\n"
                                "Upgrade: websocket\r\n"
                                "Connection: Upgrade\r\n"
                                "Sec-WebSocket-Accept: ")
                    + Protocol::acceptKeyFor(key) + "\r\n";

    if (!protocol.empty())
        response += "Sec-WebSocket-Protocol: " + protocol + "\r\n";

    return response + "\r\n";
}

Protocol::Frame webSocketServerCloseFrame(int code, std::string_view reason)
{
    auto fits = reason.size() < webSocketServerMaxCloseReason
                    ? reason.size()
                    : webSocketServerMaxCloseReason;

    return {Protocol::Opcode::close,
            true,
            Protocol::encodeClose(code, reason.substr(0, fits))};
}

// The upgrade request as the server reads it, header names lowercased the way
// HTTP matches them, so a lookup never turns on how the peer spelled one.
struct WebSocketServerRequest
{
    std::string header(const std::string& name) const
    {
        auto found = headers.find(name);
        return found != headers.end() ? found->second : std::string();
    }

    bool asksForWebSocket() const
    {
        return method == "GET" && Strings::toLower(header("upgrade")) == "websocket"
               && Strings::toLower(header("connection")).find("upgrade")
                      != std::string::npos
               && header("sec-websocket-version") == "13"
               && !header("sec-websocket-key").empty();
    }

    std::string method;
    std::string path;
    std::map<std::string, std::string> headers;
};

void webSocketServerReadRequestLine(std::string_view line,
                                    WebSocketServerRequest& request)
{
    auto afterMethod = line.find(' ');

    if (afterMethod == std::string_view::npos)
        return;

    request.method = std::string(line.substr(0, afterMethod));

    auto rest = line.substr(afterMethod + 1);
    request.path = std::string(rest.substr(0, rest.find(' ')));
}

void webSocketServerReadHeaderLine(std::string_view line,
                                   WebSocketServerRequest& request)
{
    auto colon = line.find(':');

    if (colon == std::string_view::npos)
        return;

    auto name = Strings::toLower(Strings::trim(line.substr(0, colon)));

    if (!name.empty())
        request.headers[name] = Strings::trim(line.substr(colon + 1));
}

// Where every report a client thread makes crosses to the message thread, and
// the only place the callbacks are ever called from. A report is queued
// holding a reference to this object and the generation it was made in; a
// stop() or a destructor moves the generation on, so whatever is still in the
// queue is dropped on arrival rather than reaching a Server that has finished
// with it.
class WebSocketServerEndpoint
    : public std::enable_shared_from_this<WebSocketServerEndpoint>
{
public:
    explicit WebSocketServerEndpoint(ServerCallbacks callbacksToUse)
        : callbacks(std::move(callbacksToUse))
    {
    }

    void connected(ClientId id, Handshake handshake)
    {
        deliver([id, handshake = std::move(handshake)](auto& self)
                { self.callbacks.onConnect(id, handshake); });
    }

    void received(ClientId id, Message message)
    {
        deliver([id, message = std::move(message)](auto& self)
                { self.callbacks.onMessage(id, message); });
    }

    // The slot is let go before the callback, so a clientCount() asked for
    // inside one already counts the client that is leaving as gone.
    void disconnected(ClientId id, CloseStatus status)
    {
        deliver(
            [id, status](auto& self)
            {
                self.reapClient(id);
                self.callbacks.onDisconnect(id, status);
            });
    }

    // A peer whose upgrade never succeeded owes no onDisconnect, its onConnect
    // having never been reported, but its thread still has to be joined.
    void abandoned(ClientId id)
    {
        deliver([id](auto& self) { self.reapClient(id); });
    }

    // The listener is done for. Its thread has already left the accept loop,
    // so the message thread is where it is joined and the socket let go -
    // whatever clients are on carry on untouched.
    void listenerFailed(std::string error)
    {
        deliver(
            [error = std::move(error)](auto& self)
            {
                self.dropListener();
                self.callbacks.onError(error);
            });
    }

    void setReapClient(std::function<void(ClientId)> callback)
    {
        reapClient = std::move(callback);
    }

    void setDropListener(std::function<void()> callback)
    {
        dropListener = std::move(callback);
    }

    void suspendDelivery() { generation.fetch_add(1); }

    void stopDelivering()
    {
        suspendDelivery();
        reapClient = [](ClientId) {};
        dropListener = [] {};
    }

private:
    template <typename Delivery>
    void deliver(Delivery delivery)
    {
        auto self = shared_from_this();
        auto at = generation.load();

        Threads::callAsync(
            [self, delivery = std::move(delivery), at]
            {
                if (self->generation.load() == at)
                    delivery(*self);
            });
    }

    ServerCallbacks callbacks;
    std::function<void(ClientId)> reapClient = [](ClientId) {};
    std::function<void()> dropListener = [] {};
    std::atomic<std::uint64_t> generation {0};
};

// One peer, its thread, and everything the two sides of it share. The thread
// owns the reading; the message thread writes under sendMutex, which is all a
// TCP::Connection needs to serve both at once - its send path touches neither
// the bytes the receive path buffers nor anything else the reader writes.
class WebSocketServerClient
{
public:
    WebSocketServerClient(ClientId idToUse,
                          TCP::Connection connectionToUse,
                          ServerOptions optionsToUse,
                          std::shared_ptr<WebSocketServerEndpoint> endpointToUse)
        : id(idToUse)
        , connection(std::move(connectionToUse))
        , options(std::move(optionsToUse))
        , endpoint(std::move(endpointToUse))
    {
    }

    ~WebSocketServerClient()
    {
        stopping.store(true);
        join();
    }

    WebSocketServerClient(const WebSocketServerClient&) = delete;
    WebSocketServerClient& operator=(const WebSocketServerClient&) = delete;

    void start()
    {
        worker = std::thread([this] { run(); });
    }

    void join()
    {
        if (worker.joinable())
            worker.join();
    }

    bool isConnected() const { return connected.load(); }

    void sendMessage(const Message& message)
    {
        if (!connected.load() || closeSent.load())
            return;

        auto opcode = message.type == MessageType::binary ? Protocol::Opcode::binary
                                                          : Protocol::Opcode::text;

        writeFrame({opcode, true, message.data});
    }

    // The server's half of the closing handshake: the frame goes now and the
    // thread waits out closeTimeout for the peer's answer.
    void beginClose(int code, std::string_view reason)
    {
        if (!connected.load() || closeSent.exchange(true))
            return;

        writeFrame(webSocketServerCloseFrame(code, reason));
    }

    // §7.4.1's 1001, and then no waiting at all: stop() owes its caller a
    // prompt return, not a closing handshake.
    void requestStop()
    {
        if (connected.load() && !closeSent.exchange(true))
            writeFrame(webSocketServerCloseFrame(1001, "going away"));

        stopping.store(true);
    }

private:
    void run()
    {
        auto request = WebSocketServerRequest();

        if (!readRequest(request) || !answerUpgrade(request))
        {
            finish();
            endpoint->abandoned(id);
            return;
        }

        connected.store(true);
        endpoint->connected(id, handshake);

        auto status = frameLoop();

        finish();
        endpoint->disconnected(id, status);
    }

    bool answerUpgrade(const WebSocketServerRequest& request)
    {
        if (!request.asksForWebSocket())
        {
            writeRaw(webSocketServerBadRequest());
            return false;
        }

        handshake.path = request.path;
        handshake.headers = request.headers;
        handshake.peerHost = connection.address().host;
        handshake.protocol = webSocketServerChosenProtocol(
            options.protocols, request.header("sec-websocket-protocol"));

        return writeRaw(webSocketServerHandshakeResponse(
            request.header("sec-websocket-key"), handshake.protocol));
    }

    bool readRequest(WebSocketServerRequest& request)
    {
        auto deadline = Time::Deadline {webSocketServerHandshakeTimeout};

        for (auto line = 0; line < webSocketServerMaxRequestLines; ++line)
        {
            auto text = std::string();

            if (!readLine(text, deadline))
                return false;

            if (line == 0)
                webSocketServerReadRequestLine(text, request);
            else if (text.empty())
                return true;
            else
                webSocketServerReadHeaderLine(text, request);
        }

        return false;
    }

    bool readLine(std::string& line, const Time::Deadline& deadline)
    {
        while (!stopping.load() && !deadline.expired())
        {
            try
            {
                line = connection.receiveLine();
                return true;
            }
            catch (const TCP::TimeoutError&)
            {
                continue;
            }
            catch (const TCP::Error&)
            {
                return false;
            }
        }

        return false;
    }

    CloseStatus frameLoop()
    {
        auto buffer = std::string();
        auto answering = std::optional<Time::Deadline>();

        while (!stopping.load())
        {
            if (closeSent.load() && !answering.has_value())
                answering.emplace(options.closeTimeout);

            if (answering.has_value() && answering->expired())
                return {1006, {}};

            auto chunk = std::string();

            try
            {
                chunk = connection.receive(webSocketServerReadChunk);
            }
            catch (const TCP::TimeoutError&)
            {
                continue;
            }
            catch (const TCP::Error&)
            {
                return {1006, {}};
            }

            if (chunk.empty())
                return {1006, {}};

            buffer += chunk;

            // A frame whose declared length alone is past the limit is caught
            // here, before the bytes behind it are ever waited for.
            if (buffer.size() > options.maxMessageSize + webSocketServerFrameHeader)
                return closeWith(1009, "message too big");

            if (auto settled = drainFrames(buffer); settled.has_value())
                return *settled;
        }

        return {1006, {}};
    }

    std::optional<CloseStatus> drainFrames(std::string& buffer)
    {
        while (true)
        {
            auto decoded = std::optional<Protocol::Decoded>();

            try
            {
                decoded = Protocol::decode(buffer);
            }
            catch (const Protocol::Error&)
            {
                return closeWith(1002, "protocol error");
            }

            if (!decoded.has_value())
                return std::nullopt;

            buffer.erase(0, decoded->consumed);

            if (auto settled = handleFrame(decoded->frame); settled.has_value())
                return settled;
        }
    }

    std::optional<CloseStatus> handleFrame(const Protocol::Frame& frame)
    {
        switch (frame.opcode)
        {
            case Protocol::Opcode::text:
            case Protocol::Opcode::binary:
                assembly.clear();
                assemblyType = frame.opcode == Protocol::Opcode::binary
                                   ? MessageType::binary
                                   : MessageType::text;
                return collect(frame);

            case Protocol::Opcode::continuation:
                return collect(frame);

            case Protocol::Opcode::ping:
                writeFrame({Protocol::Opcode::pong, true, frame.payload});
                return std::nullopt;

            case Protocol::Opcode::pong:
                return std::nullopt;

            case Protocol::Opcode::close:
                return peerClosed(frame.payload);
        }

        return std::nullopt;
    }

    std::optional<CloseStatus> collect(const Protocol::Frame& frame)
    {
        if (assembly.size() + frame.payload.size() > options.maxMessageSize)
            return closeWith(1009, "message too big");

        assembly += frame.payload;

        if (!frame.fin)
            return std::nullopt;

        endpoint->received(id, {std::move(assembly), assemblyType});
        assembly.clear();
        return std::nullopt;
    }

    // §5.5.1: the peer's own code and reason go back to it unchanged, unless
    // this side had already said its piece.
    CloseStatus peerClosed(const std::string& payload)
    {
        auto status = CloseStatus();

        try
        {
            status = Protocol::decodeClose(payload);
        }
        catch (const Protocol::Error&)
        {
            status = {1002, {}};
        }

        if (!closeSent.exchange(true))
            writeFrame({Protocol::Opcode::close, true, payload});

        return status;
    }

    CloseStatus closeWith(int code, const std::string& reason)
    {
        if (!closeSent.exchange(true))
            writeFrame(webSocketServerCloseFrame(code, reason));

        return {code, reason};
    }

    bool writeFrame(const Protocol::Frame& frame)
    {
        return writeRaw(Protocol::encode(frame, false));
    }

    bool writeRaw(const std::string& bytes)
    {
        auto lock = std::scoped_lock(sendMutex);

        if (finished || !connection.isOpen())
            return false;

        try
        {
            connection.send(bytes);
            return true;
        }
        catch (const TCP::Error&)
        {
            stopping.store(true);
            return false;
        }
    }

    // The socket goes under the same lock every write takes, so nothing on the
    // message thread is ever sending into a stream the thread has let go.
    void finish()
    {
        auto lock = std::scoped_lock(sendMutex);
        finished = true;
        connection.close();
    }

    ClientId id = 0;
    TCP::Connection connection;
    ServerOptions options;
    std::shared_ptr<WebSocketServerEndpoint> endpoint;

    Handshake handshake;
    std::string assembly;
    MessageType assemblyType = MessageType::text;

    std::mutex sendMutex;
    bool finished = false;

    std::atomic<bool> stopping {false};
    std::atomic<bool> connected {false};
    std::atomic<bool> closeSent {false};

    std::thread worker;
};
} // namespace

struct Server::Impl
{
    Impl(ServerCallbacks callbacks, ServerOptions optionsToUse)
        : options(std::move(optionsToUse))
        , endpoint(std::make_shared<WebSocketServerEndpoint>(std::move(callbacks)))
    {
        endpoint->setReapClient([this](ClientId id) { reap(id); });
        endpoint->setDropListener([this] { dropListener(); });
    }

    ~Impl()
    {
        stop();
        endpoint->stopDelivering();
    }

    Impl(const Impl&) = delete;
    Impl& operator=(const Impl&) = delete;

    bool listen(std::uint16_t port)
    {
        if (listener.has_value())
            return false;

        try
        {
            listener =
                TCP::Listener::bind(port, webSocketServerTimeouts(), options.bindTo);
        }
        catch (const TCP::Error&)
        {
            return false;
        }

        stopping.store(false);
        failed.store(false);
        accepting = std::thread([this] { acceptLoop(); });
        return true;
    }

    // Nothing is reported from inside here: the generation moves on last, so
    // every report the teardown itself provoked is dropped on arrival.
    void stop()
    {
        stopping.store(true);

        if (accepting.joinable())
            accepting.join();

        auto leaving = takeClients();

        for (auto& [id, client]: leaving)
            client->requestStop();

        for (auto& [id, client]: leaving)
            client->join();

        listener.reset();
        endpoint->suspendDelivery();
    }

    bool isListening() const
    {
        return listener.has_value() && !failed.load() && listener->isListening();
    }

    std::uint16_t boundPort() const
    {
        return listener.has_value() ? listener->port() : 0;
    }

    std::size_t clientCount() const
    {
        auto lock = std::scoped_lock(clientsMutex);
        auto count = std::size_t {0};

        for (const auto& [id, client]: clients)
            if (client->isConnected())
                ++count;

        return count;
    }

    void sendTo(ClientId id, const Message& message)
    {
        if (auto client = clientFor(id))
            client->sendMessage(message);
    }

    void broadcast(const Message& message)
    {
        for (auto& [id, client]: snapshot())
            client->sendMessage(message);
    }

    void closeClient(ClientId id, int code, std::string_view reason)
    {
        if (auto client = clientFor(id))
            client->beginClose(code, reason);
    }

private:
    using ClientPointer = std::shared_ptr<WebSocketServerClient>;
    using ClientMap = std::map<ClientId, ClientPointer>;

    // A listener that fails is not something to loop over: the thread leaves,
    // isListening() answers false from here, and the report carries why.
    void acceptLoop()
    {
        while (!stopping.load())
        {
            try
            {
                adopt(listener->accept());
            }
            catch (const TCP::TimeoutError&)
            {
                continue;
            }
            catch (const TCP::Error& error)
            {
                if (stopping.load())
                    return;

                failed.store(true);
                endpoint->listenerFailed(error.what());
                return;
            }
        }
    }

    // Runs on the message thread, the accept thread having already left.
    void dropListener()
    {
        if (accepting.joinable())
            accepting.join();

        listener.reset();
    }

    // The lock spans the client's thread starting, so a report racing back
    // from it cannot reach a map entry whose thread is not there to join yet.
    void adopt(TCP::Connection peer)
    {
        auto lock = std::scoped_lock(clientsMutex);
        auto id = nextId++;

        auto client = std::make_shared<WebSocketServerClient>(
            id, std::move(peer), options, endpoint);

        clients[id] = client;
        client->start();
    }

    void reap(ClientId id)
    {
        auto leaving = ClientPointer();

        {
            auto lock = std::scoped_lock(clientsMutex);
            auto found = clients.find(id);

            if (found == clients.end())
                return;

            leaving = std::move(found->second);
            clients.erase(found);
        }

        leaving->join();
    }

    ClientPointer clientFor(ClientId id) const
    {
        auto lock = std::scoped_lock(clientsMutex);
        auto found = clients.find(id);
        return found != clients.end() ? found->second : ClientPointer();
    }

    ClientMap snapshot() const
    {
        auto lock = std::scoped_lock(clientsMutex);
        return clients;
    }

    ClientMap takeClients()
    {
        auto lock = std::scoped_lock(clientsMutex);
        auto taken = std::move(clients);
        clients.clear();
        return taken;
    }

    ServerOptions options;
    std::shared_ptr<WebSocketServerEndpoint> endpoint;

    std::optional<TCP::Listener> listener;
    std::thread accepting;
    std::atomic<bool> stopping {false};
    std::atomic<bool> failed {false};

    mutable std::mutex clientsMutex;
    ClientMap clients;
    ClientId nextId = 1;
};

Server::Server(ServerCallbacks callbacks, ServerOptions options)
{
    Threads::assertMainThread();
    impl.create(std::move(callbacks), std::move(options));
}

Server::~Server() = default;

bool Server::listen(std::uint16_t port)
{
    Threads::assertMainThread();
    return impl->listen(port);
}

void Server::stop()
{
    Threads::assertMainThread();
    impl->stop();
}

bool Server::isListening() const
{
    return impl->isListening();
}

std::uint16_t Server::boundPort() const
{
    return impl->boundPort();
}

std::size_t Server::clientCount() const
{
    return impl->clientCount();
}

void Server::send(ClientId client, std::string_view text)
{
    Threads::assertMainThread();
    impl->sendTo(client, {std::string(text), MessageType::text});
}

void Server::sendBinary(ClientId client, std::string_view bytes)
{
    Threads::assertMainThread();
    impl->sendTo(client, {std::string(bytes), MessageType::binary});
}

void Server::broadcast(std::string_view text)
{
    Threads::assertMainThread();
    impl->broadcast({std::string(text), MessageType::text});
}

void Server::close(ClientId client, int code, std::string_view reason)
{
    Threads::assertMainThread();
    impl->closeClient(client, code, reason);
}

} // namespace eacp::WebSocket

#include "HttpServer.h"
#include "HttpServerDispatcher.h"

#include "../HTTP/HttpProtocol.h"
#include "../TCP/Listener.h"

#include <atomic>
#include <mutex>
#include <thread>

// The HTTP server on every platform but Apple: an accept thread over a
// TCP::Listener and one thread per connection until its request is read, the
// response then going out through the dispatcher. Apple's HttpServer.mm is the
// same server driven by CFSocket sources on the main run loop instead.
namespace eacp::HTTP
{

namespace
{

// stop() interrupts the listener and every connection still being read - a
// browser's idle speculative one among them, held open for as long as it
// likes - so this is only a backstop for a wake-up a platform loses.
constexpr auto httpServerSlice = Time::MS {100};

constexpr auto httpServerReadChunk = 4096;
constexpr auto streamingBodyReadChunk = 64 * 1024;

TCP::Timeouts httpServerTimeouts()
{
    return {httpServerSlice, httpServerSlice};
}

} // namespace

struct Server::Impl
{
    explicit Impl(ServerOptions opts)
        : options(opts)
        , dispatcher(makeDispatcher(opts))
    {
    }

    ~Impl();

    bool start(int port, RequestHandler h, StreamingRouteFinder finder);
    void stop();

    void acceptLoop();
    void handleConnection(TCP::Connection connection);
    std::optional<std::string> receiveFromPeer(TCP::Connection& connection,
                                               int maxBytes);
    std::optional<Request>
        readRequest(TCP::Connection& connection,
                    RequestParser& parser,
                    std::optional<StreamingRequestHandler>& streamingHandler);
    void dispatchRequest(TCP::Connection connection, Request request);
    void serveStreamingConnection(TCP::Connection& connection,
                                  const Request& head,
                                  std::string bytesAfterHeaders,
                                  const StreamingRequestHandler& streamingHandler);

    bool beginReading(TCP::Connection& connection);
    void endReading(TCP::Connection& connection);
    void interruptReading();

    ServerOptions options;
    OwningPointer<Dispatcher> dispatcher;
    std::optional<TCP::Listener> listener;
    RequestHandler handler;
    StreamingRouteFinder findStreamingRoute;
    std::thread acceptThread;
    std::atomic<bool> running {false};

    std::mutex clientMutex;
    Vector<std::thread> clientThreads;
    Vector<TCP::Connection*> reading;
};

Server::Server(ServerOptions options)
    : impl(makeOwned<Impl>(options))
{
}

Server::~Server() = default;

bool Server::startListening(int port,
                            RequestHandler handler,
                            StreamingRouteFinder findStreamingRoute)
{
    return impl->start(port, std::move(handler), std::move(findStreamingRoute));
}

void Server::stop()
{
    impl->stop();
}

int Server::boundPort() const
{
    return impl->listener.has_value() ? (int) impl->listener->port() : -1;
}

Server::Impl::~Impl()
{
    stop();
}

bool Server::Impl::start(int port, RequestHandler h, StreamingRouteFinder finder)
{
    if (listener.has_value())
        return false;

    try
    {
        listener = TCP::Listener::bind(
            (std::uint16_t) port, httpServerTimeouts(), options.bindTo);
    }
    catch (const TCP::Error&)
    {
        return false;
    }

    handler = std::move(h);
    findStreamingRoute = std::move(finder);
    running = true;
    acceptThread = std::thread([this] { acceptLoop(); });
    return true;
}

void Server::Impl::stop()
{
    interruptReading();

    if (listener.has_value())
        listener->interrupt();

    if (acceptThread.joinable())
        acceptThread.join();

    listener.reset();

    auto threadsToJoin = Vector<std::thread>();
    {
        auto lock = std::lock_guard(clientMutex);
        threadsToJoin = std::move(clientThreads);
    }
    for (auto& t: threadsToJoin)
        if (t.joinable())
            t.join();

    if (dispatcher)
        dispatcher->shutdown();
}

void Server::Impl::acceptLoop()
{
    while (running)
    {
        auto peer = std::optional<TCP::Connection>();

        try
        {
            peer = listener->accept();
        }
        catch (const TCP::TimeoutError&)
        {
            continue;
        }
        catch (const TCP::Error&)
        {
            return;
        }

        auto lock = std::lock_guard(clientMutex);
        clientThreads.emplace_back([this, connection = std::move(*peer)]() mutable
                                   { handleConnection(std::move(connection)); });
    }
}

// The reply is sent from whichever thread the dispatcher runs the handler on,
// and takes as long as the peer takes to drain it: the slice that let stop()
// interrupt an idle read is not a bound a response should carry.
void Server::Impl::dispatchRequest(TCP::Connection connection, Request request)
{
    connection.setIoTimeout({});
    auto peer = std::make_shared<TCP::Connection>(std::move(connection));

    auto sendReplyAndClose = [peer](const Response& res)
    {
        try
        {
            sendResponse(res, [&peer](std::string_view bytes) { peer->send(bytes); });
        }
        catch (const TCP::Error&)
        {
        }

        peer->close();
    };

    dispatcher->dispatch(
        DispatchTask {std::move(request), handler, std::move(sendReplyAndClose)});
}

// A connection is listed only while its request is read, under the lock
// stop() interrupts the list under, so stop() never reaches a stream that has
// moved on to its reply or one whose thread has finished with it.
bool Server::Impl::beginReading(TCP::Connection& connection)
{
    auto lock = std::lock_guard(clientMutex);

    if (!running)
        return false;

    reading.add(&connection);
    return true;
}

void Server::Impl::endReading(TCP::Connection& connection)
{
    auto isThisOne = [&connection](TCP::Connection* candidate)
    { return candidate == &connection; };

    auto lock = std::lock_guard(clientMutex);
    reading.eraseIf(isThisOne);
}

void Server::Impl::interruptReading()
{
    auto lock = std::lock_guard(clientMutex);
    running = false;

    for (auto* connection: reading)
        connection->interrupt();
}

void Server::Impl::handleConnection(TCP::Connection connection)
{
    if (!beginReading(connection))
        return;

    auto parser = RequestParser();
    auto streamingHandler = std::optional<StreamingRequestHandler>();
    auto request = readRequest(connection, parser, streamingHandler);

    if (request.has_value() && streamingHandler.has_value())
    {
        serveStreamingConnection(
            connection, *request, parser.takeBytesAfterHeaders(), *streamingHandler);
        return;
    }

    endReading(connection);

    if (request.has_value())
        dispatchRequest(std::move(connection), std::move(*request));
}

// The connection stays listed for stop() while the handler reads the body,
// and leaves the list only to send the reply, as a dispatched one does.
void Server::Impl::serveStreamingConnection(
    TCP::Connection& connection,
    const Request& head,
    std::string bytesAfterHeaders,
    const StreamingRequestHandler& streamingHandler)
{
    auto peer = PeerStream();
    peer.alreadyReceived = std::move(bytesAfterHeaders);
    peer.receiveMore = [this, &connection]
    { return receiveFromPeer(connection, streamingBodyReadChunk).value_or(""); };
    peer.send = [&connection](std::string_view bytes) { connection.send(bytes); };

    auto response = Response();

    try
    {
        response = serveStreamingRequest(streamingHandler, head, std::move(peer));
    }
    catch (const TCP::Error&)
    {
        endReading(connection);
        return;
    }

    endReading(connection);
    connection.setIoTimeout({});

    try
    {
        sendResponse(response,
                     [&connection](std::string_view bytes) { connection.send(bytes); });
    }
    catch (const TCP::Error&)
    {
    }

    connection.close();
}

std::optional<std::string> Server::Impl::receiveFromPeer(TCP::Connection& connection,
                                                         int maxBytes)
{
    while (true)
    {
        try
        {
            auto chunk = connection.receive(maxBytes);

            if (chunk.empty())
                return std::nullopt;

            return chunk;
        }
        catch (const TCP::TimeoutError&)
        {
            if (!running)
                return std::nullopt;
        }
        catch (const TCP::Error&)
        {
            return std::nullopt;
        }
    }
}

std::optional<Request> Server::Impl::readRequest(
    TCP::Connection& connection,
    RequestParser& parser,
    std::optional<StreamingRequestHandler>& streamingHandler)
{
    auto hasLookedForStreamingRoute = false;

    while (true)
    {
        auto chunk = receiveFromPeer(connection, httpServerReadChunk);

        if (!chunk.has_value())
            return std::nullopt;

        auto state = parser.feed(chunk->data(), (int) chunk->size());

        if (state == RequestParser::State::Invalid)
            return std::nullopt;

        if (parser.hasHeaders() && !hasLookedForStreamingRoute)
        {
            hasLookedForStreamingRoute = true;
            streamingHandler = findStreamingRoute(parser.request());
        }

        if (state == RequestParser::State::Ready || streamingHandler.has_value())
        {
            auto request = std::move(parser.request());
            request.remoteAddr = connection.address().host;
            request.remotePort = connection.address().port;

            if (streamingHandler.has_value())
                request.body.clear();

            return request;
        }
    }
}

} // namespace eacp::HTTP

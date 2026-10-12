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

// Every blocking call the server makes wakes this often, so stop() is bounded
// by one slice rather than by a browser's idle speculative connection, which
// it holds open for as long as it likes.
constexpr auto httpServerSlice = Time::MS {250};

constexpr auto httpServerReadChunk = 4096;

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

    bool start(int port, RequestHandler h);
    void stop();

    void acceptLoop();
    void handleConnection(TCP::Connection connection);
    void dispatchRequest(TCP::Connection connection, Request request);

    ServerOptions options;
    OwningPointer<Dispatcher> dispatcher;
    std::optional<TCP::Listener> listener;
    RequestHandler handler;
    std::thread acceptThread;
    std::atomic<bool> running {false};

    std::mutex clientMutex;
    Vector<std::thread> clientThreads;
};

Server::Server(ServerOptions options)
    : impl(makeOwned<Impl>(options))
{
}

Server::~Server() = default;

bool Server::listen(int port, RequestHandler handler)
{
    return impl->start(port, std::move(handler));
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

bool Server::Impl::start(int port, RequestHandler h)
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
    running = true;
    acceptThread = std::thread([this] { acceptLoop(); });
    return true;
}

void Server::Impl::stop()
{
    running = false;

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

    auto sendResponse = [peer](const Response& res)
    {
        try
        {
            peer->send(serializeResponse(res));
        }
        catch (const TCP::Error&)
        {
        }

        peer->close();
    };

    dispatcher->dispatch(
        DispatchTask {std::move(request), handler, std::move(sendResponse)});
}

void Server::Impl::handleConnection(TCP::Connection connection)
{
    auto parser = RequestParser();

    while (true)
    {
        auto chunk = std::string();

        try
        {
            chunk = connection.receive(httpServerReadChunk);
        }
        catch (const TCP::TimeoutError&)
        {
            if (running)
                continue;

            return;
        }
        catch (const TCP::Error&)
        {
            return;
        }

        if (chunk.empty())
            return;

        auto state = parser.feed(chunk.data(), (int) chunk.size());

        if (state == RequestParser::State::Invalid)
            return;

        if (state == RequestParser::State::Ready)
        {
            auto request = std::move(parser.request());
            request.remoteAddr = connection.address().host;
            request.remotePort = connection.address().port;
            dispatchRequest(std::move(connection), std::move(request));
            return;
        }
    }
}

} // namespace eacp::HTTP

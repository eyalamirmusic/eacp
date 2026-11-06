#pragma once

/** @file A small HTTP server with method-and-path routing. */

#include "../HTTP/Http.h"

namespace eacp::HTTP
{

using RequestHandler = std::function<Response(const Request&)>;

/// The body of a request on a streaming route. The server has read only the
/// request line and headers when the handler runs, and the handler pulls the
/// body through this, so it can answer 413 or 400 before any of it is read.
struct RequestBodyReader
{
    virtual ~RequestBodyReader() = default;

    /// The client's Content-Length, or nullopt for a chunked body.
    virtual std::optional<std::int64_t> getDeclaredLength() const = 0;

    /// Up to maxBytes more of the body, decoded from chunks when it is chunked,
    /// and empty once the whole body has been read. Throws Error with 400 when
    /// the chunk framing is malformed or the client closes before the end.
    virtual std::string readSome(std::size_t maxBytes) = 0;
};

/// Runs on the connection's own thread, never through the dispatcher: it
/// blocks on the socket for as long as the client takes to send the body.
using StreamingRequestHandler =
    std::function<Response(const Request&, RequestBodyReader&)>;

/// Where request handlers run: EventLoop on the main thread, ThreadPool on
/// threadPoolSize worker threads, concurrently.
enum class ServerThreadingMode
{
    EventLoop,
    ThreadPool,
};

struct ServerOptions
{
    ServerThreadingMode threading = ServerThreadingMode::EventLoop;
    int threadPoolSize = 4;
    BindInterface bindTo = BindInterface::loopback;
};

/// Thrown from a handler to answer with this status and response instead of
/// a 500. Any other exception becomes a plain-text 500.
struct Error : std::runtime_error
{
    Error(int statusCodeToUse, const std::string& message);
    explicit Error(Response responseToSend);

    int statusCode = 500;
    Response response;
};

/// Throws Error with a plain-text body.
[[noreturn]] void throwError(const std::string& message, int statusCode = 400);

/// Finds the streaming handler for a request whose headers have been read,
/// nullopt when the request is to be read whole.
using StreamingRouteFinder =
    std::function<std::optional<StreamingRequestHandler>(const Request&)>;

template <typename Handler>
struct RouteTable
{
    void add(const std::string& method, const std::string& path, Handler handler)
    {
        constexpr auto wildcardSuffix = std::string_view("/*");

        if (path.ends_with(wildcardSuffix))
            prefixes[method + " " + path.substr(0, path.size() - 1)] =
                std::move(handler);
        else
            exact[method + " " + path] = std::move(handler);
    }

    std::optional<Handler> find(const std::string& method,
                                const std::string& path) const
    {
        auto key = method + " " + path;

        if (auto match = exact.find(key); match != exact.end())
            return match->second;

        auto longestMatch = std::optional<Handler>();
        auto longestLength = std::size_t {0};

        for (const auto& [prefix, handler]: prefixes)
        {
            if (key.starts_with(prefix) && key.size() > prefix.size()
                && prefix.size() > longestLength)
            {
                longestMatch = handler;
                longestLength = prefix.size();
            }
        }

        return longestMatch;
    }

private:
    std::map<std::string, Handler> exact;
    std::map<std::string, Handler> prefixes;
};

/// An HTTP server with exact method-and-path routes; an unmatched request
/// gets a 404. Bound to loopback unless ServerOptions::bindTo says any.
struct Server
{
    explicit Server(ServerOptions options = {});
    ~Server();

    // A path ending in "/*" matches every path under it ("/objects/*" takes
    // "/objects/a" and "/objects/a/parts/1", not "/objects"); an exact path
    // wins over a wildcard, and the longest wildcard wins over shorter ones.
    // The handler reads its own path from the request.
    void addRoute(const std::string& method,
                  const std::string& path,
                  RequestHandler handler);

    void get(const std::string& path, RequestHandler handler);
    void post(const std::string& path, RequestHandler handler);
    void put(const std::string& path, RequestHandler handler);
    void del(const std::string& path, RequestHandler handler);

    /// The body is read by the handler as it needs it, through the reader.
    void addStreamingRoute(const std::string& method,
                           const std::string& path,
                           StreamingRequestHandler handler);

    /// Starts serving the routes added so far; routes added later are not
    /// seen. A request matching a streaming route is streamed, every other one
    /// is read whole first. Port 0 picks a free port (read it from
    /// boundPort()). False when the port cannot be bound.
    bool listen(int port);

    /// One handler for every request, each read whole; routes are not used.
    bool listen(int port, RequestHandler handler);

    int boundPort() const;

    void stop();

private:
    RequestHandler buildRouteHandler() const;
    StreamingRouteFinder buildStreamingRouteFinder() const;
    bool startListening(int port,
                        RequestHandler handler,
                        StreamingRouteFinder findStreamingRoute);

    RouteTable<RequestHandler> routes;
    RouteTable<StreamingRequestHandler> streamingRoutes;

    struct Impl;
    OwningPointer<Impl> impl;
};

} // namespace eacp::HTTP

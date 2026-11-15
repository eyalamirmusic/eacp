#include "HttpServer.h"
#include "HttpServerDispatcher.h"
#include "ResponseBuilders.h"

namespace eacp::HTTP
{

namespace
{
Response handleRouteMatch(const RouteTable<RequestHandler>& routes,
                          const Request& req)
{
    try
    {
        auto match = routes.find(req.type, req.pathWithoutQuery());

        if (!match)
            return makePlainTextResponse(404, "Not Found");

        return (*match)(req);
    }
    catch (const Error& e)
    {
        return e.response;
    }
    catch (const std::exception& e)
    {
        return makePlainTextResponse(500, e.what());
    }
}
} // namespace

Error::Error(int statusCodeToUse, const std::string& message)
    : std::runtime_error(message)
    , statusCode(statusCodeToUse)
    , response(makePlainTextResponse(statusCodeToUse, message))
{
}

Error::Error(Response responseToSend)
    : std::runtime_error(responseToSend.content)
    , statusCode(responseToSend.statusCode)
    , response(std::move(responseToSend))
{
}

void throwError(const std::string& message, int statusCode)
{
    throw Error(statusCode, message);
}

void Server::addRoute(const std::string& method,
                      const std::string& path,
                      RequestHandler handler)
{
    routes.add(method, path, std::move(handler));
}

void Server::addStreamingRoute(const std::string& method,
                               const std::string& path,
                               StreamingRequestHandler handler)
{
    streamingRoutes.add(method, path, std::move(handler));
}

void Server::get(const std::string& path, RequestHandler handler)
{
    addRoute("GET", path, std::move(handler));
}

void Server::post(const std::string& path, RequestHandler handler)
{
    addRoute("POST", path, std::move(handler));
}

void Server::put(const std::string& path, RequestHandler handler)
{
    addRoute("PUT", path, std::move(handler));
}

void Server::del(const std::string& path, RequestHandler handler)
{
    addRoute("DELETE", path, std::move(handler));
}

bool Server::listen(int port)
{
    return startListening(port, buildRouteHandler(), buildStreamingRouteFinder());
}

bool Server::listen(int port, RequestHandler handler)
{
    auto findsNoStreamingRoute = [](const Request&)
    { return std::optional<StreamingRequestHandler>(); };

    return startListening(port, std::move(handler), findsNoStreamingRoute);
}

RequestHandler Server::buildRouteHandler() const
{
    auto routesCopy = routes;

    return [routesCopy](const Request& req) -> Response
    { return handleRouteMatch(routesCopy, req); };
}


StreamingRouteFinder Server::buildStreamingRouteFinder() const
{
    auto routesCopy = streamingRoutes;

    return [routesCopy](const Request& req)
    { return routesCopy.find(req.type, req.pathWithoutQuery()); };
}

Response serveStreamingRequest(const StreamingRequestHandler& handler,
                               const Request& head,
                               PeerStream peer)
{
    try
    {
        auto body = StreamingBodyDecoder(head, std::move(peer));
        return handler(head, body);
    }
    catch (const Error& e)
    {
        return e.response;
    }
    catch (const std::exception& e)
    {
        return makePlainTextResponse(500, e.what());
    }
}

} // namespace eacp::HTTP

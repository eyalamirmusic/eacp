#pragma once

#include <eacp/Network/HTTP/Http.h>

#include <eacp/Core/Utils/Containers.h>
#include <functional>
#include <map>
#include <stdexcept>
#include <string>

namespace eacp::HTTP
{

using RequestHandler = std::function<Response(const Request&)>;

enum class ServerThreadingMode
{
    EventLoop,
    ThreadPool,
};

struct ServerOptions
{
    ServerThreadingMode threading = ServerThreadingMode::EventLoop;
    int threadPoolSize = 4;
};

struct Error : std::runtime_error
{
    Error(int statusCodeToUse, const std::string& message);
    explicit Error(Response responseToSend);

    int statusCode = 500;
    Response response;
};

[[noreturn]] void throwError(const std::string& message, int statusCode = 400);

struct Server
{
    explicit Server(ServerOptions options = {});
    ~Server();

    void addRoute(const std::string& method,
                  const std::string& path,
                  RequestHandler handler);

    void get(const std::string& path, RequestHandler handler);
    void post(const std::string& path, RequestHandler handler);
    void put(const std::string& path, RequestHandler handler);
    void del(const std::string& path, RequestHandler handler);

    bool listen(int port);
    bool listen(int port, RequestHandler handler);

    int boundPort() const;

    void stop();

private:
    RequestHandler buildRouteHandler() const;

    std::map<std::string, RequestHandler> routes;

    struct Impl;
    OwningPointer<Impl> impl;
};

} // namespace eacp::HTTP

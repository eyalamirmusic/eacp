#pragma once

#include <eacp/Network/HTTP/Http.h>

#include <functional>
#include <memory>

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

struct Server
{
    explicit Server(ServerOptions options = {});
    ~Server();

    bool listen(int port, RequestHandler handler);
    void stop();

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};

} // namespace eacp::HTTP

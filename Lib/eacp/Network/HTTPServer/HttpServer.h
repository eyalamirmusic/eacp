#pragma once

#include <eacp/Network/HTTP/Http.h>

#include <functional>
#include <memory>

namespace eacp::HTTP
{

using RequestHandler = std::function<Response(const Request&)>;

struct Server
{
    Server();
    ~Server();

    bool listen(int port, RequestHandler handler);
    void stop();

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};

} // namespace eacp::HTTP

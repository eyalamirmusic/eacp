#pragma once

#include <eacp/Network/Network.h>

#include <Miro/Bridge.h>

namespace eacp::HTTP::Rpc
{

// Mounts a Miro::Bridge onto an HTTP::Server as one POST endpoint at
// `basePath`, speaking the WebView bridge's wire protocol. Must outlive the
// HTTP::Server it attached to, since the route handler captures `this`.
class Server
{
public:
    Server(eacp::HTTP::Server& server,
           Miro::Bridge& bridge,
           std::string basePath = "/rpc");

    Server(const Server&) = delete;
    Server& operator=(const Server&) = delete;

private:
    Response handle(const Request& req);

    Miro::Bridge& bridge;
    std::string basePath;
};

} // namespace eacp::HTTP::Rpc

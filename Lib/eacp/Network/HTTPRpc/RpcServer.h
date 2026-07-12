#pragma once

#include <eacp/Network/Network.h>

#include <Miro/Bridge.h>

namespace eacp::HTTP::Rpc
{

// Mounts a Miro::Bridge onto an HTTP::Server as a single POST endpoint at
// `basePath` (default "/rpc"). Wire protocol mirrors the WebView bridge:
// request { "command", "payload" }, success { "result": <JSON> }, error the
// handler's status code (404 for unknown commands) with { "error": "..." }.
//
// The caller owns the Bridge and can wire it to a WebView transport at the
// same time, serving one set of typed handlers over both wires.
//
// The Server must outlive the HTTP::Server it attached to — the route handler
// captures `this`.
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

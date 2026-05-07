#pragma once

#include <eacp/Network/HTTPServer/HttpServer.h>

#include <functional>
#include <memory>

namespace eacp::HTTP
{

using SendResponseFn = std::function<void(const Response&)>;

struct DispatchTask
{
    Request request;
    RequestHandler handler;
    SendResponseFn sendResponse;
};

struct Dispatcher
{
    virtual ~Dispatcher() = default;
    virtual void dispatch(DispatchTask task) = 0;
    virtual void shutdown() = 0;
};

std::unique_ptr<Dispatcher> makeDispatcher(const ServerOptions& options);

} // namespace eacp::HTTP

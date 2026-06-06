#pragma once

#include <eacp/Network/HTTPServer/HttpServer.h>

#include <eacp/Core/Utils/Containers.h>
#include <functional>

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

OwningPointer<Dispatcher> makeDispatcher(const ServerOptions& options);

} // namespace eacp::HTTP

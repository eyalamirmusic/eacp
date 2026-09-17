#pragma once

#include "HttpServer.h"

namespace eacp::HTTP
{

using SendResponseFn = std::function<void(const Response&)>;

struct DispatchTask
{
    Request request;
    RequestHandler handler = [](const eacp::HTTP::Request&)
    { return eacp::HTTP::Response {}; };
    SendResponseFn sendResponse = [](const eacp::HTTP::Response&) {};
};

struct Dispatcher
{
    virtual ~Dispatcher() = default;
    virtual void dispatch(DispatchTask task) = 0;
    virtual void shutdown() = 0;
};

OwningPointer<Dispatcher> makeDispatcher(const ServerOptions& options);

} // namespace eacp::HTTP

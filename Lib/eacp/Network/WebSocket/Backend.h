#pragma once

#include "WebSocket.h"

#include <memory>

namespace eacp::WebSocket
{

// What a backend reports, from any thread - the caller's inside makeBackend
// included. The common layer marshals each report to the message thread and
// ignores whatever arrives after the Connection is gone, so a report may
// come late; but a backend reports a terminal event - closed or failed -
// exactly once, and nothing after it.
class Sink
{
public:
    virtual ~Sink() = default;

    virtual void opened(const std::string& protocol) = 0;
    virtual void received(Message message) = 0;
    virtual void closed(int code, const std::string& reason) = 0;
    virtual void failed(const std::string& error) = 0;
};

// One platform's transport, connecting from construction. send and close are
// called on the message thread only, close at most once; after close the
// backend keeps receiving until the peer's close frame or the transport's
// end, then reports it. The destructor tears the transport down without
// waiting on the network.
class Backend
{
public:
    virtual ~Backend() = default;

    virtual void send(const Message& message) = 0;
    virtual void close(int code, const std::string& reason) = 0;
};

std::unique_ptr<Backend> makeBackend(const std::string& url,
                                     const Options& options,
                                     std::shared_ptr<Sink> sink);

bool backendIsSupported();

} // namespace eacp::WebSocket

#pragma once

/** @file How the HTTP server runs a request handler: on the event loop or a thread pool. */

#include "HttpServer.h"

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

/// A dispatcher for options.threading: main-thread or thread-pool.
OwningPointer<Dispatcher> makeDispatcher(const ServerOptions& options);

// How a streaming body reaches the decoder: the bytes the header read already
// pulled past the blank line, then more from the socket on demand.
struct PeerStream
{
    std::string alreadyReceived;

    // The next bytes from the client, empty once it has closed its side.
    std::function<std::string()> receiveMore;

    std::function<void(std::string_view)> send;
};

// Decodes a Content-Length or chunked body as the handler asks for it, and
// answers "Expect: 100-continue" on the first read, so a client that waits
// for it sends nothing until the handler has accepted the request.
struct StreamingBodyDecoder : RequestBodyReader
{
    // Throws Error with 400 for a Content-Length that is not a number.
    StreamingBodyDecoder(const Request& head, PeerStream peerToUse);

    std::optional<std::int64_t> getDeclaredLength() const override;
    std::string readSome(std::size_t maxBytes) override;

private:
    void sendContinueIfExpected();
    void receiveAtLeast(std::size_t byteCount);
    std::string takeBuffered(std::size_t byteCount);
    std::string readLine();
    std::string readSomeOfChunkedBody(std::size_t maxBytes);
    void beginNextChunk();

    PeerStream peer;
    std::string buffered;
    std::optional<std::int64_t> declaredLength;
    std::int64_t remainingInPiece = 0;
    bool isChunked = false;
    bool expectsContinue = false;
    bool isFinished = false;
};

// Runs a streaming handler over a request whose headers have been read,
// turning an Error it throws into its response and anything else into 500.
Response serveStreamingRequest(const StreamingRequestHandler& handler,
                               const Request& head,
                               PeerStream peer);

// Sends the status line and headers, then the body: content, or the file at
// contentFilePath read in slices.
void sendResponse(const Response& response,
                  const std::function<void(std::string_view)>& send);

} // namespace eacp::HTTP

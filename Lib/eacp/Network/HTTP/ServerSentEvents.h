#pragma once

#include "Http.h"

#include <functional>
#include <optional>
#include <string_view>

namespace eacp::HTTP
{

// One dispatched Server-Sent Event.
struct ServerSentEvent
{
    // "message" unless the stream named one, matching the EventSource
    // default rather than leaving callers to special-case an empty string.
    std::string event = "message";

    std::string data;
    std::string id;

    // The stream asking to be reconnected to after this many milliseconds.
    std::optional<int> retryMs;
};

// Incremental Server-Sent Events reader.
//
// Pure: it takes bytes and emits events, so it can be exercised without a
// socket - which is the point, since the failure modes worth testing are
// about chunk boundaries, and those are the hardest thing to reproduce
// against a live server.
//
// Feed it whatever arrives, in whatever sizes it arrives. Line endings may
// be LF, CRLF or CR, and any of them may be split across two chunks.
class SseParser
{
public:
    void feed(std::string_view chunk);

    // Flushes a final event that arrived without its terminating blank
    // line - what a server that closes the connection mid-frame leaves
    // behind. Discards a partial field line rather than emitting a
    // truncated value.
    void finish();

    void reset();

    std::function<void(const ServerSentEvent&)> onEvent =
        [](const ServerSentEvent&) {};

private:
    void handleLine(std::string_view line);
    void handleField(std::string_view field, std::string_view value);
    void dispatch();

    std::string buffer;
    ServerSentEvent pending;
    bool hasData = false;
};

// Runs req as a streamed request and reports each event as it arrives.
// Blocks until the stream ends, and carries the same threading contract as
// ChunkCallback: onEvent runs off the transport's thread, not the caller's.
Response streamServerSentEvents(
    const Request& req, const std::function<void(const ServerSentEvent&)>& onEvent);

} // namespace eacp::HTTP

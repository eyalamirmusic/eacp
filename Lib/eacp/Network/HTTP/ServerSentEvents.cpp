#include "ServerSentEvents.h"

namespace eacp::HTTP
{

void SseParser::feed(std::string_view chunk)
{
    buffer.append(chunk);

    auto pos = std::size_t {0};

    while (true)
    {
        auto at = buffer.find_first_of("\r\n", pos);

        if (at == std::string::npos)
            break;

        // A carriage return at the very end may be the first half of a CRLF
        // whose newline is in the next chunk. Treating it as a line ending
        // now would split one line into two, so wait for more bytes.
        if (buffer[at] == '\r' && at + 1 == buffer.size())
            break;

        auto line = std::string_view(buffer).substr(pos, at - pos);
        pos = at + 1;

        if (buffer[at] == '\r' && pos < buffer.size() && buffer[pos] == '\n')
            ++pos;

        handleLine(line);
    }

    buffer.erase(0, pos);
}

void SseParser::finish()
{
    if (!buffer.empty())
    {
        // A trailing line with no terminator is incomplete by definition;
        // emitting it would hand the caller a value the server had not
        // finished writing.
        buffer.clear();
    }

    dispatch();
}

void SseParser::reset()
{
    buffer.clear();
    pending = ServerSentEvent();
    hasData = false;
}

void SseParser::handleLine(std::string_view line)
{
    if (line.empty())
    {
        dispatch();
        return;
    }

    if (line.front() == ':')
        return;

    auto colon = line.find(':');

    if (colon == std::string_view::npos)
    {
        handleField(line, {});
        return;
    }

    auto value = line.substr(colon + 1);

    if (!value.empty() && value.front() == ' ')
        value.remove_prefix(1);

    handleField(line.substr(0, colon), value);
}

void SseParser::handleField(std::string_view field, std::string_view value)
{
    if (field == "event")
    {
        pending.event = std::string(value);
        return;
    }

    if (field == "data")
    {
        // Every data line contributes its own newline, and dispatch strips
        // exactly one at the end. Joining with a separator instead would
        // lose the blank line that a trailing empty data field encodes.
        pending.data.append(value);
        pending.data.push_back('\n');
        hasData = true;
        return;
    }

    if (field == "id")
    {
        // A null byte in an id is the one value the spec says to ignore
        // outright rather than store.
        if (value.find('\0') == std::string_view::npos)
            pending.id = std::string(value);

        return;
    }

    if (field == "retry")
    {
        for (auto c: value)
            if (c < '0' || c > '9')
                return;

        if (value.empty() || value.size() > 9)
            return;

        pending.retryMs = std::stoi(std::string(value));
    }
}

void SseParser::dispatch()
{
    // An event with no data line carries nothing to act on, and the spec
    // says to drop it rather than deliver an empty one. The id and retry it
    // may have set are already recorded and survive into the next event.
    if (!hasData)
    {
        pending.event = "message";
        return;
    }

    // The final newline of a multi-line data field belongs to the framing,
    // not the payload.
    if (!pending.data.empty() && pending.data.back() == '\n')
        pending.data.pop_back();

    onEvent(pending);

    pending.event = "message";
    pending.data.clear();
    pending.retryMs.reset();
    hasData = false;
}

Response streamServerSentEvents(
    const Request& req, const std::function<void(const ServerSentEvent&)>& onEvent)
{
    auto parser = SseParser();
    parser.onEvent = onEvent;

    auto response =
        req.stream([&parser](std::string_view chunk) { parser.feed(chunk); });

    parser.finish();

    return response;
}

} // namespace eacp::HTTP

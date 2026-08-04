#pragma once

#include "Http.h"

namespace eacp::HTTP
{

std::string findHeaderIgnoringCase(const std::map<std::string, std::string>& headers,
                                   const std::string& key);

// Splits one "Name: value" line on its first colon and stores the trimmed
// halves. Lines with no colon, and lines whose name is empty, are ignored - both
// are what a status line or a stray CRLF looks like. Every backend reads its
// header block from somewhere different (curl callback, WinHTTP raw block, our
// own socket parser) but they all arrive at this one line format.
void addHeaderLine(std::string_view line,
                   std::map<std::string, std::string>& headers);

bool acceptsByteRanges(const std::string& acceptRangesHeaderValue);

std::string serializeResponse(const Response& response);

class RequestParser
{
public:
    enum class State
    {
        NeedMore,
        Ready,
        Invalid
    };

    State feed(const char* data, std::size_t length);
    Request& request() { return parsed; }

private:
    State tryParseHeaders();
    void readContentLengthFromHeaders();
    bool isBodyComplete() const;
    State finishIfBodyComplete();

    std::string buffer;
    Request parsed;
    std::size_t bodyStart = 0;
    std::size_t bodyExpected = 0;
    bool headersParsed = false;
};

} // namespace eacp::HTTP

#pragma once

#include "Http.h"

namespace eacp::HTTP
{

std::string findHeaderIgnoringCase(const std::map<std::string, std::string>& headers,
                                   const std::string& key);

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

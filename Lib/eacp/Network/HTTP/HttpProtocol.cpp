#include "HttpProtocol.h"

#include <sstream>

namespace eacp::HTTP
{

namespace
{

const char* reasonPhraseForStatus(int code)
{
    switch (code)
    {
        case 200:
            return "OK";
        case 201:
            return "Created";
        case 204:
            return "No Content";
        case 301:
            return "Moved Permanently";
        case 302:
            return "Found";
        case 304:
            return "Not Modified";
        case 400:
            return "Bad Request";
        case 401:
            return "Unauthorized";
        case 403:
            return "Forbidden";
        case 404:
            return "Not Found";
        case 500:
            return "Internal Server Error";
        default:
            return "OK";
    }
}

bool responseHasContentLength(const Response& response)
{
    for (const auto& [name, value]: response.headers)
        if (Strings::equalsCaseInsensitive(name, "Content-Length"))
            return true;

    return false;
}

void writeStatusLine(std::stringstream& out, const Response& response)
{
    auto code = response.statusCode != 0 ? response.statusCode : 200;
    out << "HTTP/1.1 " << code << " " << reasonPhraseForStatus(code) << "\r\n";
}

void writeHeaders(std::stringstream& out, const Response& response)
{
    for (const auto& [name, value]: response.headers)
        out << name << ": " << value << "\r\n";

    if (!responseHasContentLength(response))
        out << "Content-Length: " << response.content.size() << "\r\n";

    out << "Connection: close\r\n\r\n";
}

void stripTrailingCarriageReturn(std::string& line)
{
    if (!line.empty() && line.back() == '\r')
        line.pop_back();
}

bool parseRequestLine(const std::string& line, Request& request)
{
    auto firstSpace = line.find(' ');
    auto secondSpace = line.find(' ', firstSpace + 1);

    if (firstSpace == std::string::npos || secondSpace == std::string::npos)
        return false;

    request.type = line.substr(0, firstSpace);
    request.url = line.substr(firstSpace + 1, secondSpace - firstSpace - 1);
    return true;
}

void parseQueryParamsFromUrl(Request& request)
{
    auto questionMark = request.url.find('?');

    if (questionMark != std::string::npos)
        request.params = parseQueryString(request.url.substr(questionMark + 1));
}

void parseHeaderLines(std::stringstream& stream, Request& request)
{
    auto line = std::string();

    while (std::getline(stream, line))
    {
        stripTrailingCarriageReturn(line);

        if (line.empty())
            break;

        auto colon = line.find(':');

        if (colon == std::string::npos)
            continue;

        auto name = Strings::trim(line.substr(0, colon));
        auto value = Strings::trim(line.substr(colon + 1));
        request.headers[name] = value;
    }
}

} // namespace

std::string findHeaderIgnoringCase(const std::map<std::string, std::string>& headers,
                                   const std::string& key)
{
    for (const auto& [name, value]: headers)
        if (Strings::equalsCaseInsensitive(name, key))
            return value;

    return {};
}

bool acceptsByteRanges(const std::string& acceptRangesHeaderValue)
{
    return Strings::toLower(acceptRangesHeaderValue).find("bytes")
           != std::string::npos;
}

std::string serializeResponse(const Response& response)
{
    auto out = std::stringstream();

    writeStatusLine(out, response);
    writeHeaders(out, response);
    out << response.content;

    return out.str();
}

RequestParser::State RequestParser::feed(const char* data, std::size_t length)
{
    buffer.append(data, length);

    if (!headersParsed)
    {
        auto state = tryParseHeaders();

        if (state != State::Ready)
            return state;
    }

    return finishIfBodyComplete();
}

RequestParser::State RequestParser::tryParseHeaders()
{
    auto headerEnd = buffer.find("\r\n\r\n");

    if (headerEnd == std::string::npos)
        return State::NeedMore;

    auto headerSection = std::stringstream(buffer.substr(0, headerEnd));
    auto requestLine = std::string();

    if (!std::getline(headerSection, requestLine))
        return State::Invalid;

    stripTrailingCarriageReturn(requestLine);

    if (!parseRequestLine(requestLine, parsed))
        return State::Invalid;

    parseQueryParamsFromUrl(parsed);
    parseHeaderLines(headerSection, parsed);

    bodyStart = headerEnd + 4;
    headersParsed = true;
    readContentLengthFromHeaders();

    return State::Ready;
}

void RequestParser::readContentLengthFromHeaders()
{
    auto contentLength = findHeaderIgnoringCase(parsed.headers, "Content-Length");

    if (contentLength.empty())
        return;

    try
    {
        bodyExpected = (std::size_t) std::stoul(contentLength);
    }
    catch (...)
    {
    }
}

bool RequestParser::isBodyComplete() const
{
    return buffer.size() - bodyStart >= bodyExpected;
}

RequestParser::State RequestParser::finishIfBodyComplete()
{
    if (!isBodyComplete())
        return State::NeedMore;

    parsed.body = buffer.substr(bodyStart, bodyExpected);
    return State::Ready;
}

} // namespace eacp::HTTP

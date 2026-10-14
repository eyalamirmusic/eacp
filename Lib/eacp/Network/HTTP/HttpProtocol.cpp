#include "HttpProtocol.h"

#include <initializer_list>
#include <sstream>
#include <vector>

namespace eacp::HTTP
{

namespace
{

// A body past this cannot be indexed with int offsets, so a Content-Length
// beyond it is clamped rather than believed.
constexpr auto largestBody = std::int64_t {0x7FFFFFFF};

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
        case 206:
            return "Partial Content";
        case 301:
            return "Moved Permanently";
        case 302:
            return "Found";
        case 303:
            return "See Other";
        case 304:
            return "Not Modified";
        case 307:
            return "Temporary Redirect";
        case 308:
            return "Permanent Redirect";
        case 416:
            return "Range Not Satisfiable";
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

        addHeaderLine(line, request.headers);
    }
}

} // namespace

void addHeaderLine(std::string_view line,
                   std::map<std::string, std::string>& headers)
{
    auto colon = line.find(':');

    if (colon == std::string_view::npos)
        return;

    auto name = Strings::trim(line.substr(0, colon));

    if (name.empty())
        return;

    headers[std::move(name)] = Strings::trim(line.substr(colon + 1));
}

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

bool ResponseSizeLimit::exceeds(int64_t bytes) const
{
    return maxBytes > 0 && bytes > maxBytes;
}

bool ResponseSizeLimit::add(int64_t more)
{
    received += more;
    return !exceeds(received);
}

std::string responseTooLargeError(int64_t maxBytes)
{
    return "The response exceeded " + std::to_string(maxBytes) + " bytes";
}

std::string timedOutError()
{
    return "The request timed out";
}

std::string tooManyRedirectsError(int maxRedirects)
{
    if (maxRedirects < 0)
        return "The request was redirected too many times";

    return "The request was redirected more than " + std::to_string(maxRedirects)
           + " times";
}

void preferIdentityEncoding(Request& req)
{
    if (!req.hasHeader("Accept-Encoding"))
        req.headers["Accept-Encoding"] = "identity";
}

bool asksForIdentityEncoding(const Request& req)
{
    return Strings::toLower(Strings::trim(req.getHeader("Accept-Encoding")))
           == "identity";
}

bool isSchemeDowngrade(const std::string& from, const std::string& to)
{
    return Strings::toLower(from).starts_with("https:")
           && Strings::toLower(to).starts_with("http:");
}

Time::MS connectLimit(const Request& req)
{
    return req.connectTimeout.count > 0 ? req.connectTimeout : req.timeout;
}

Time::MS idleLimit(const Request& req)
{
    return req.idleTimeout.count > 0 ? req.idleTimeout : req.timeout;
}

bool needsDefaultUserAgent(const Request& req)
{
    return !req.hasHeader("User-Agent");
}

bool isRedirectStatus(int statusCode)
{
    switch (statusCode)
    {
        case 301:
        case 302:
        case 303:
        case 307:
        case 308:
            return true;
        default:
            return false;
    }
}

namespace
{
std::size_t endOfScheme(const std::string& url)
{
    auto separator = url.find("://");
    return separator == std::string::npos ? 0 : separator + 1;
}

std::size_t endOfAuthority(const std::string& url)
{
    auto schemeEnd = endOfScheme(url);
    auto authorityStart = schemeEnd == 0 ? 0 : schemeEnd + 2;
    auto pathStart = url.find_first_of("/?#", authorityStart);
    return pathStart == std::string::npos ? url.size() : pathStart;
}

std::string directoryOf(const std::string& url)
{
    auto authorityEnd = endOfAuthority(url);
    auto pathEnd = url.find_first_of("?#", authorityEnd);
    auto path = url.substr(authorityEnd, pathEnd - authorityEnd);
    auto lastSlash = path.rfind('/');

    if (lastSlash == std::string::npos)
        return url.substr(0, authorityEnd) + "/";

    return url.substr(0, authorityEnd) + path.substr(0, lastSlash + 1);
}

bool hasScheme(const std::string& location)
{
    auto colon = location.find(':');
    auto slash = location.find('/');
    return colon != std::string::npos && colon > 0
           && (slash == std::string::npos || colon < slash);
}

std::int64_t parseByteCount(const std::string& text)
{
    if (text.empty() || text == "*")
        return -1;

    try
    {
        return std::stoll(text);
    }
    catch (...)
    {
        return -1;
    }
}
} // namespace

namespace
{
// RFC 3986 section 5.2.4: "." segments go, ".." takes the one before it with
// it, and a path that ended on either still ends in a slash.
std::string removeDotSegments(const std::string& path)
{
    auto segments = std::vector<std::string>();
    auto endsInDirectory = false;
    auto pos = path.starts_with("/") ? std::size_t {1} : std::size_t {0};

    while (pos <= path.size())
    {
        auto slash = path.find('/', pos);
        auto end = slash == std::string::npos ? path.size() : slash;
        auto segment = path.substr(pos, end - pos);
        auto isLast = slash == std::string::npos;

        if (segment == "..")
        {
            if (!segments.empty())
                segments.pop_back();
        }
        else if (segment != ".")
        {
            segments.push_back(segment);
        }

        endsInDirectory = isLast && (segment == "." || segment == "..");

        if (isLast)
            break;

        pos = slash + 1;
    }

    auto result = std::string(path.starts_with("/") ? "/" : "");

    for (auto i = std::size_t {0}; i < segments.size(); ++i)
    {
        if (i > 0)
            result.push_back('/');

        result += segments[i];
    }

    if (endsInDirectory && !result.ends_with("/"))
        result.push_back('/');

    return result;
}

std::string withoutDotSegments(const std::string& url)
{
    auto authorityEnd = endOfAuthority(url);
    auto pathEnd = url.find_first_of("?#", authorityEnd);

    if (pathEnd == std::string::npos)
        pathEnd = url.size();

    auto path = url.substr(authorityEnd, pathEnd - authorityEnd);
    return url.substr(0, authorityEnd) + removeDotSegments(path)
           + url.substr(pathEnd);
}

std::string mergedLocation(const std::string& from, const std::string& location)
{
    if (hasScheme(location))
        return location;

    if (location.starts_with("//"))
        return from.substr(0, endOfScheme(from)) + location;

    if (location.front() == '#')
        return from.substr(0, from.find('#')) + location;

    if (location.front() == '?')
    {
        auto queryStart = from.find_first_of("?#", endOfAuthority(from));
        return from.substr(0, queryStart) + location;
    }

    if (location.front() == '/')
        return from.substr(0, endOfAuthority(from)) + location;

    return directoryOf(from) + location;
}
} // namespace

std::string resolveRedirectLocation(const std::string& from,
                                    const std::string& location)
{
    if (location.empty())
        return from;

    return withoutDotSegments(mergedLocation(from, location));
}

namespace
{
std::string originOf(const std::string& url)
{
    return Strings::toLower(url.substr(0, endOfAuthority(url)));
}

bool turnsIntoGet(const Request& from, int statusCode)
{
    if (statusCode == 303)
        return from.type != "HEAD";

    return (statusCode == 301 || statusCode == 302) && from.type == "POST";
}

void eraseHeaderIgnoringCase(std::map<std::string, std::string>& headers,
                             const std::string& key)
{
    std::erase_if(headers,
                  [&key](const auto& header)
                  { return Strings::equalsCaseInsensitive(header.first, key); });
}
} // namespace

Request redirectedRequest(const Request& from,
                          int statusCode,
                          const std::string& location)
{
    auto next = from;
    next.url = resolveRedirectLocation(from.url, location);

    if (turnsIntoGet(from, statusCode))
    {
        next.type = "GET";
        next.body.clear();
        eraseHeaderIgnoringCase(next.headers, "Content-Type");
        eraseHeaderIgnoringCase(next.headers, "Content-Length");
    }

    if (originOf(next.url) != originOf(from.url))
        for (auto* credential: {"Authorization", "Cookie", "Proxy-Authorization"})
            eraseHeaderIgnoringCase(next.headers, credential);

    return next;
}

ContentRange parseContentRange(const std::string& value)
{
    auto range = ContentRange();
    auto text = Strings::trim(value);

    if (!Strings::toLower(text).starts_with("bytes "))
        return range;

    text = Strings::trim(text.substr(6));

    auto slash = text.find('/');
    if (slash == std::string::npos)
        return range;

    range.total = parseByteCount(Strings::trim(text.substr(slash + 1)));

    auto span = Strings::trim(text.substr(0, slash));
    auto dash = span.find('-');

    if (dash == std::string::npos)
        return range;

    range.first = parseByteCount(span.substr(0, dash));
    range.last = parseByteCount(span.substr(dash + 1));
    return range;
}

DownloadWrite downloadWriteFor(int statusCode,
                               const std::map<std::string, std::string>& headers,
                               std::int64_t resumeFrom)
{
    if (resumeFrom <= 0)
        return DownloadWrite::Replace;

    if (statusCode == 206)
    {
        auto range =
            parseContentRange(findHeaderIgnoringCase(headers, "Content-Range"));
        return range.first == resumeFrom ? DownloadWrite::Append
                                         : DownloadWrite::Keep;
    }

    if (statusCode >= 200 && statusCode < 300)
        return DownloadWrite::Replace;

    return DownloadWrite::Keep;
}

std::string serializeResponse(const Response& response)
{
    auto out = std::stringstream();

    writeStatusLine(out, response);
    writeHeaders(out, response);
    out << response.content;

    return out.str();
}

Request& RequestParser::request()
{
    return parsed;
}

RequestParser::State RequestParser::feed(const char* data, int length)
{
    buffer.append(data, (std::size_t) length);

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

    bodyStart = (int) headerEnd + 4;
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
        auto declared = std::stoll(contentLength);
        bodyExpected = declared > 0
                           ? (int) (declared < largestBody ? declared : largestBody)
                           : 0;
    }
    catch (...)
    {
    }
}

bool RequestParser::isBodyComplete() const
{
    return (int) buffer.size() - bodyStart >= bodyExpected;
}

RequestParser::State RequestParser::finishIfBodyComplete()
{
    if (!isBodyComplete())
        return State::NeedMore;

    parsed.body = buffer.substr((std::size_t) bodyStart, (std::size_t) bodyExpected);
    return State::Ready;
}

} // namespace eacp::HTTP

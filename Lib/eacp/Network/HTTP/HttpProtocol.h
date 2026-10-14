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

struct ResponseSizeLimit
{
    int64_t maxBytes = 0;
    int64_t received = 0;

    bool exceeds(int64_t bytes) const;
    bool add(int64_t more);
};

std::string responseTooLargeError(int64_t maxBytes);
std::string timedOutError();
std::string tooManyRedirectsError(int maxRedirects);

// The limits each backend applies for Request::connectTimeout and
// idleTimeout: the field itself, else the whole-request timeout, else zero
// for the backend's own default.
Time::MS connectLimit(const Request& req);
Time::MS idleLimit(const Request& req);

bool needsDefaultUserAgent(const Request& req);

// A byte range names offsets in the representation the server sends, so a
// ranged request asks for it unencoded - unless the caller chose an
// Accept-Encoding - and a backend that would otherwise decode transparently
// leaves it alone.
void preferIdentityEncoding(Request& req);
bool asksForIdentityEncoding(const Request& req);

// An https URL redirecting to plain http, which a backend that follows
// redirects itself refuses as HttpURLConnection does.
bool isSchemeDowngrade(const std::string& from, const std::string& to);

bool isRedirectStatus(int statusCode);

// Where a Location header points, read against the URL that answered with it:
// absolute, scheme-relative, host-relative or path-relative.
std::string resolveRedirectLocation(const std::string& from,
                                    const std::string& location);

// The request a redirect leads to, for a backend that follows them itself:
// a 303, and a 301 or 302 answering a POST, become a GET without the body,
// and Authorization, Cookie and Proxy-Authorization are dropped when the
// redirect leaves the origin.
Request redirectedRequest(const Request& from,
                          int statusCode,
                          const std::string& location);

struct ContentRange
{
    std::int64_t first = -1;
    std::int64_t last = -1;
    std::int64_t total = -1;
};

// "bytes 100-199/1000", "bytes 100-199/*" or "bytes */1000"; anything
// unreadable is left at -1.
ContentRange parseContentRange(const std::string& value);

// What a download does with the file already at its destination, once the
// final status is known. resumeFrom is the size of the file when the request
// asked for its remainder, and zero for a fresh download, which always
// replaces it.
enum class DownloadWrite
{
    Replace,
    Append,
    Keep
};

DownloadWrite downloadWriteFor(int statusCode,
                               const std::map<std::string, std::string>& headers,
                               std::int64_t resumeFrom);

// Each backend's single-stream download. resumeFrom is the number of bytes
// already in the file - the request then carries the matching Range header -
// and the backend decides through downloadWriteFor whether the body replaces
// the file, is appended to it or is dropped. Progress counts resumeFrom in
// when the body is appended. Never sets DownloadProgress::done; downloadFile
// does that.
Response downloadFileFrom(const Request& req,
                          const std::string& filePath,
                          std::int64_t resumeFrom);

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

    State feed(const char* data, int length);
    Request& request();

private:
    State tryParseHeaders();
    void readContentLengthFromHeaders();
    bool isBodyComplete() const;
    State finishIfBodyComplete();

    std::string buffer;
    Request parsed;
    int bodyStart = 0;
    int bodyExpected = 0;
    bool headersParsed = false;
};

} // namespace eacp::HTTP

#pragma once

#include "../Common.h"

#include <atomic>
#include <map>

namespace eacp::HTTP
{

struct Response
{
    void setContent(const std::string& contentToUse, const std::string& contentType);
    void setHeader(const std::string& key, const std::string& value);
    void setRedirect(const std::string& url, int status = 302);

    std::string content;
    std::string error;
    std::map<std::string, std::string> headers;
    int statusCode = 0;
};

struct DownloadProgress
{
    std::atomic<std::int64_t> bytesReceived {0};
    std::atomic<std::int64_t> totalBytes {-1};
    std::atomic<bool> cancel {false};
    std::atomic<bool> done {false};
};

struct FormField
{
    std::string name;
    std::string value;
};

// One multipart file part. It either names a file, read when the request is
// performed, or carries the bytes itself - for a payload rendered in memory,
// which would otherwise need a temporary file just to be uploaded.
struct FileField
{
    static FileField
        fromBytes(const std::string& fieldName,
                  const std::string& fileName,
                  std::string bytes,
                  const std::string& contentType = "application/octet-stream");

    std::string fieldName;
    std::string filePath;
    std::string contentType = "application/octet-stream";
    std::string fileName;
    std::string content;
    bool inMemory = false;
};

// Whether a request may be answered from a cache on this machine.
// IgnoreLocalCache, the default, always reaches the server, and is what every
// backend does. UseLocalCache only changes anything where the platform keeps
// a cache: NSURLCache on Apple, and on Android an HttpResponseCache the app has
// installed itself. WinHTTP and libcurl have no cache, so there it is the
// default by another name.
enum class CachePolicy
{
    IgnoreLocalCache,
    UseLocalCache
};

struct Request
{
    Request(const std::string& urlToUse = "");

    static Request post(const std::string& urlToUse = "",
                        const std::string& bodyToUse = {});

    Request& addFormField(const std::string& name, const std::string& value);
    Request&
        addFileField(const std::string& fieldName,
                     const std::string& filePath,
                     const std::string& contentType = "application/octet-stream");
    Request&
        addFileBytes(const std::string& fieldName,
                     const std::string& fileName,
                     std::string bytes,
                     const std::string& contentType = "application/octet-stream");

    Response perform() const;
    Response downloadTo(const std::string& filePath) const;

    bool hasHeader(const std::string& key) const;
    std::string getHeader(const std::string& key) const;

    bool hasParam(const std::string& key) const;
    std::string getParam(const std::string& key) const;

    std::string pathWithoutQuery() const;

    std::string url;
    std::string type = "GET";
    std::string body;
    std::map<std::string, std::string> headers;

    // Wall-clock limit for the whole request. Zero, the default, leaves each
    // backend's own behaviour in place. A parallel download applies it to
    // every chunk separately.
    Time::MS timeout {0};

    // Limit on establishing the connection. Zero, the default, leaves it to
    // the backend - or to `timeout`, when that is set. NSURLSession reports no
    // connect event, so on Apple platforms this bounds the time until the
    // response headers arrive or the first byte of the body is sent: the
    // connect, TLS and the server's think time together.
    Time::MS connectTimeout {0};

    // Longest silence tolerated once the request has started: the transfer
    // fails when no bytes arrive for this long, however long it has been
    // running. It does not cap the total, so a slow but steady download
    // survives it. Zero, the default, leaves it to the backend - or to
    // `timeout`, when that is set.
    Time::MS idleTimeout {0};

    // Whether a 3xx answer is followed to its Location. Off, the redirect
    // itself is the response - its status, headers and body - and nothing
    // is sent to the new URL, so a header such as an API key never leaves
    // the host it was meant for.
    bool followRedirects = true;

    // Most redirects followed before the request fails with an error, when
    // followRedirects is on. Negative, the default, leaves the backend's own
    // limit in place; zero fails on the first redirect.
    int maxRedirects = -1;

    CachePolicy cachePolicy = CachePolicy::IgnoreLocalCache;

    // Downloads only: when the destination already holds N bytes, asks for
    // the rest with "Range: bytes=N-", unencoded unless the request names an
    // Accept-Encoding. A 206 starting at N is appended, progress counts the N
    // bytes already there, and a file that is then whole is reported as 200;
    // a 206 that stops short of the end is an error, the file keeping the
    // bytes it did get for a later resume. A 200 replaces the file. A
    // 416 whose Content-Range says the file is already whole leaves it as it
    // is and reports 200; any other 416, or a 206 starting elsewhere, discards
    // the partial file and downloads it again from the start. Any other
    // answer - a 404, say - is returned with the partial file left untouched.
    // Add an If-Range header to have a changed resource sent whole. A
    // download that resumes is never split into parallel chunks.
    bool resumeDownload = false;

    // Most bytes of body accepted, zero for no limit. A response that grows
    // past it is aborted as it arrives - not buffered and measured after -
    // and reported as an error with no status.
    int64_t maxResponseSize = 0;

    Vector<FormField> formFields;
    Vector<FileField> fileFields;

    std::map<std::string, std::string> params;
    std::string remoteAddr;
    int remotePort = -1;

    DownloadProgress* progress = nullptr;
    int parallelChunks = 1;
};

Response httpRequest(const Request& req);
Response downloadFile(const Request& req, const std::string& filePath);

// "eacp/<version>": the User-Agent sent by default where the platform has
// none of its own - WinHTTP and libcurl.
std::string eacpUserAgent();

// Sets the User-Agent every request sends, on every platform, unless it
// carries a User-Agent header of its own, which always wins - say
// "MyApp/1.2" once at startup. Empty, the initial value, puts the platform's
// default back. Safe to call while requests run on other threads; a request
// already sent keeps the agent it went out with.
void setDefaultUserAgent(const std::string& agent);

// The User-Agent a request with no User-Agent header is sent with: the one
// given to setDefaultUserAgent, else eacpUserAgent() on Windows and Linux.
// Empty on Apple platforms and Android when none was set: the request then
// adds no header, and the system sends its own (CFNetwork's
// "<App>/<version> CFNetwork/... Darwin/...", or HttpURLConnection's
// http.agent, "Dalvik/...").
std::string defaultUserAgent();

// Percent-encodes every byte outside RFC 3986's unreserved set (A-Z a-z 0-9
// - _ . ~), whatever the locale, space included, and as %20 rather than '+'
// so the result is as valid in a path segment as it is in a query value.
std::string urlEncode(const std::string& text);

// Reads both spellings back: %XX escapes and the form encoding's '+'.
std::string urlDecode(const std::string& encoded);

struct QueryParameter
{
    std::string name;
    std::string value;

    bool operator==(const QueryParameter&) const = default;
};

// Decoded pairs in the order they appear, repeats and all.
Vector<QueryParameter> parseQueryParameters(const std::string& query);

// The same pairs as a map: a repeated name keeps its last value.
std::map<std::string, std::string> parseQueryString(const std::string& query);

// An address and its query, kept apart so parameters are added unencoded and
// encoded once, in order, when the string is asked for. The base is scheme,
// host and path, already in URL form and taken verbatim. A base that still
// carries a '?' - set directly, or "x?" with nothing after it - has the
// parameters joined on with '&'.
struct URL
{
    // The same as parse(text).
    URL(const std::string& text = {});

    // The fragment starts at the first '#', and the query at the first '?'
    // before it. The query's pairs are decoded the way urlDecode reads them,
    // '+' as a space included, so a literal plus must arrive as %2B - which
    // is how toString writes one. A name with no '=' comes back with an
    // empty value, and is written back as "name=".
    static URL parse(const std::string& text);

    URL& addParameter(const std::string& name, const std::string& value);

    std::string getParameter(const std::string& name) const;
    bool hasParameter(const std::string& name) const;

    std::string query() const;
    std::string toString() const;

    bool operator==(const URL&) const = default;

    std::string base;
    Vector<QueryParameter> parameters;
    std::string fragment;
};
} // namespace eacp::HTTP

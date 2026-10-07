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

    // Whether a 3xx answer is followed to its Location. Off, the redirect
    // itself is the response - its status, headers and body - and nothing
    // is sent to the new URL, so a header such as an API key never leaves
    // the host it was meant for.
    bool followRedirects = true;

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

// Percent-encodes everything outside RFC 3986's unreserved set (A-Z a-z 0-9
// - _ . ~), space included, and as %20 rather than '+' so the result is as
// valid in a path segment as it is in a query value.
std::string urlEncode(const std::string& text);

// Reads both spellings back: %XX escapes and the form encoding's '+'.
std::string urlDecode(const std::string& encoded);
std::map<std::string, std::string> parseQueryString(const std::string& query);
} // namespace eacp::HTTP

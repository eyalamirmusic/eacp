#pragma once

#include "../Common.h"

#include <atomic>
#include <functional>
#include <map>
#include <string_view>

namespace eacp::HTTP
{

// Receives the response body in the pieces it arrives in.
//
// Called on whichever thread the platform's transport delivers on, which is
// not the caller's. What is guaranteed is the lifetime: the calling thread
// stays blocked inside stream() until the body ends, and no chunk is
// delivered after it returns - so a callback capturing a local is safe, and
// one touching shared state needs its own synchronisation.
using ChunkCallback = std::function<void(std::string_view)>;

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

struct FileField
{
    std::string fieldName;
    std::string filePath;
    std::string contentType = "application/octet-stream";
    std::string fileName;
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

    Response perform() const;
    Response downloadTo(const std::string& filePath) const;

    // Hands the body to onChunk as it arrives rather than buffering it,
    // and leaves Response::content empty. Returns once the body ends, so
    // the headers and status on the result are final. This is what a
    // long-lived response - an SSE subscription, a token stream - needs:
    // perform() would not return until the server finished, which for
    // those is the point at which the answer is no longer useful.
    //
    // Set progress to stop a stream that has no end of its own: cancel is
    // checked as each chunk arrives, and bytesReceived counts the body.
    Response stream(const ChunkCallback& onChunk) const;

    bool hasHeader(const std::string& key) const;
    std::string getHeader(const std::string& key) const;

    bool hasParam(const std::string& key) const;
    std::string getParam(const std::string& key) const;

    std::string pathWithoutQuery() const;

    std::string url;
    std::string type = "GET";
    std::string body;
    std::map<std::string, std::string> headers;
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
Response httpStreamRequest(const Request& req, const ChunkCallback& onChunk);

std::string urlDecode(const std::string& encoded);
std::map<std::string, std::string> parseQueryString(const std::string& query);
} // namespace eacp::HTTP

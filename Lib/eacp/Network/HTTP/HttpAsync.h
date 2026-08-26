#pragma once

#include "Http.h"

namespace eacp::HTTP
{

using ResponseCallback = std::function<void(const Response&)>;

// A request paired with the file its body should land in. Everything a
// Request carries - timeout, headers, parallelChunks - applies as usual.
struct DownloadRequest : Request
{
    DownloadRequest() = default;
    DownloadRequest(const std::string& urlToUse, const std::string& filePathToUse);

    std::string filePath;
};

// Runs the request on a worker thread and hands the Response to `callback`
// on the message thread.
void asyncRequest(const Request& req, const ResponseCallback& callback);
void asyncDownload(const DownloadRequest& req, const ResponseCallback& callback);

// Abandons every request still in flight: no callback runs after this
void cancelAllAsyncRequests();

} // namespace eacp::HTTP

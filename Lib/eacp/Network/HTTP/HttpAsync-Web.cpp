#include "HttpAsync.h"
#include "HttpProtocol.h"
#include "HttpWeb.h"

#include <memory>

// fetch() is asynchronous already, so there is no worker: only the registry
// cancelAllAsyncRequests needs.
namespace eacp::HTTP
{
namespace
{
struct WebAsyncRequest
{
    ResponseCallback callback = [](const Response&) {};
    DownloadProgress progress;
    bool cancelled = false;
};

using WebAsyncRequestPtr = std::shared_ptr<WebAsyncRequest>;

Vector<WebAsyncRequestPtr>& webAsyncRequests()
{
    static auto requests = Vector<WebAsyncRequestPtr> {};
    return requests;
}

WebAsyncRequestPtr addWebAsyncRequest(const ResponseCallback& callback)
{
    auto entry = std::make_shared<WebAsyncRequest>();
    entry->callback = callback;
    webAsyncRequests().add(entry);
    return entry;
}

WebHttpDone deliverWebAsync(const WebAsyncRequestPtr& entry)
{
    return [entry](const Response& response)
    {
        if (entry->cancelled)
            return;

        webAsyncRequests().removeAllMatches(entry);
        entry->callback(response);
    };
}

// The entry's own progress block is how a cancel reaches the transfer.
Request preparedWebAsync(const Request& req, WebAsyncRequest& entry)
{
    auto prepared = req;
    buildMultipartBody(prepared);
    prepared.progress = &entry.progress;
    return prepared;
}
} // namespace

void asyncRequest(const Request& req, const ResponseCallback& callback)
{
    auto entry = addWebAsyncRequest(callback);
    startWebRequest(preparedWebAsync(req, *entry), deliverWebAsync(entry));
}

void asyncDownload(const DownloadRequest& req, const ResponseCallback& callback)
{
    auto entry = addWebAsyncRequest(callback);
    auto deliver = deliverWebAsync(entry);

    if (req.filePath.empty())
    {
        auto response = Response {};
        response.error = "No destination file path";
        Threads::callAsync([deliver, response] { deliver(response); });
        return;
    }

    auto prepared = req;
    prepared.progress = &entry->progress;
    startWebDownload(prepared, req.filePath, deliver);
}

void cancelAllAsyncRequests()
{
    for (const auto& entry: webAsyncRequests())
    {
        entry->cancelled = true;
        entry->progress.cancel.store(true);
    }

    webAsyncRequests().clear();
}
} // namespace eacp::HTTP

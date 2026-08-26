#include "HttpAsync.h"

#include <mutex>
#include <thread>

namespace eacp::HTTP
{

DownloadRequest::DownloadRequest(const std::string& urlToUse,
                                 const std::string& filePathToUse)
    : Request(urlToUse)
    , filePath(filePathToUse)
{
}

namespace
{
using PerformFn = std::function<Response(const Request&)>;

// One request in flight: the callback waiting on the message thread, the
// progress block the worker writes into - which is how a cancel reaches a
// transfer already under way - and the flag that keeps the callback from
// running once that cancel has happened.
struct InFlight
{
    DownloadProgress progress;
    ResponseCallback callback;
    std::atomic<bool> cancelled {false};
};

using InFlightPtr = std::shared_ptr<InFlight>;

// Owns every request from the moment it starts until its callback has run,
// so callers hold nothing, free nothing, and cannot outlive their own reply
// without saying so.
class AsyncRequests
{
public:
    InFlightPtr add(const ResponseCallback& callback)
    {
        auto entry = std::make_shared<InFlight>();
        entry->callback = callback;

        auto lock = std::lock_guard(mutex);
        running.add(entry);

        return entry;
    }

    void remove(const InFlightPtr& entry)
    {
        auto lock = std::lock_guard(mutex);
        running.removeAllMatches(entry);
    }

    void cancelAll()
    {
        auto lock = std::lock_guard(mutex);

        for (const auto& entry: running)
        {
            entry->cancelled.store(true);
            entry->progress.cancel.store(true);
        }

        running.clear();
    }

private:
    std::mutex mutex;
    Vector<InFlightPtr> running;
};

AsyncRequests& getAsyncRequests()
{
    return Singleton::getImmortal<AsyncRequests>();
}

Response performCatching(const PerformFn& perform, const Request& req)
{
    try
    {
        return perform(req);
    }
    catch (const std::exception& e)
    {
        auto response = Response();
        response.error = e.what();
        return response;
    }
}

void deliver(const InFlightPtr& entry, const Response& response)
{
    if (entry->cancelled.load())
        return;

    entry->callback(response);
    getAsyncRequests().remove(entry);
}

void start(const Request& req,
           const PerformFn& perform,
           const ResponseCallback& callback)
{
    auto entry = getAsyncRequests().add(callback);

    auto work = [entry, req, perform]
    {
        auto prepared = req;
        prepared.progress = &entry->progress;

        auto response = performCatching(perform, prepared);

        // Queued whether or not a loop is running yet: both backends buffer
        // until one is, so a request that beats the caller back to the loop
        // is delivered rather than dropped.
        auto onMessageThread = [entry, response] { deliver(entry, response); };
        Threads::callAsync(onMessageThread);
    };

    auto worker = std::thread(work);
    worker.detach();
}
} // namespace

void asyncRequest(const Request& req, const ResponseCallback& callback)
{
    auto perform = [](const Request& prepared) { return prepared.perform(); };
    start(req, perform, callback);
}

void asyncDownload(const DownloadRequest& req, const ResponseCallback& callback)
{
    auto filePath = req.filePath;

    auto perform = [filePath](const Request& prepared)
    {
        if (filePath.empty())
        {
            auto response = Response();
            response.error = "No destination file path";
            return response;
        }

        return prepared.downloadTo(filePath);
    };

    start(req, perform, callback);
}

void cancelAllAsyncRequests()
{
    Threads::assertMainThread();
    getAsyncRequests().cancelAll();
}

} // namespace eacp::HTTP

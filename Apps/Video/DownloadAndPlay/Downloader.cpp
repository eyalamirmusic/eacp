#include "Downloader.h"

#include <eacp/Core/Utils/StdPath.h>

namespace VideoDemo
{
using namespace eacp;

Downloader::~Downloader()
{
    // The worker writes through `progress`, so it must be gone before we are.
    cancel();
    join();

    // Anything already posted to the main thread finds this false and stops.
    *alive = false;
}

void Downloader::join()
{
    if (worker.joinable())
        worker.join();

    running = false;
}

void Downloader::cancel()
{
    progress->cancel = true;
}

std::int64_t Downloader::bytesReceived() const
{
    return progress->bytesReceived.load();
}

std::int64_t Downloader::totalBytes() const
{
    return progress->totalBytes.load();
}

float Downloader::fraction() const
{
    auto total = totalBytes();

    if (total <= 0)
        return -1.0f;

    return (float) ((double) bytesReceived() / (double) total);
}

bool Downloader::start(std::string url,
                       FilePath path,
                       std::function<void(Result)> onFinished)
{
    if (running)
        return false;

    // A finished-but-unjoined worker from a previous transfer.
    join();

    // A fresh block per transfer: a UI may still be reading the old one.
    progress = std::make_shared<HTTP::DownloadProgress>();
    running = true;

    worker = std::thread {
        [this,
         guard = alive,
         tracker = progress,
         url = std::move(url),
         path = std::move(path),
         onFinished = std::move(onFinished)]
        {
            auto request = HTTP::Request {url};
            request.progress = tracker.get();

            auto partial = FilePath {path.str() + ".part"};
            auto response = request.downloadTo(partial.str());

            auto cancelled = tracker->cancel.load();
            auto ok =
                !cancelled && response.error.empty() && response.statusCode == 200;

            auto ignored = std::error_code {};

            if (ok)
                std::filesystem::rename(
                    toStdPath(partial), toStdPath(path), ignored);
            else
                std::filesystem::remove(toStdPath(partial), ignored);

            auto result = Result {};
            result.ok = ok;
            result.cancelled = cancelled;
            result.path = path;

            if (!ok && !cancelled)
                result.error = response.error.empty()
                                   ? "HTTP " + std::to_string(response.statusCode)
                                   : response.error;

            Threads::callAsync(
                [this, guard, result = std::move(result), onFinished]
                {
                    if (!*guard)
                        return;

                    running = false;
                    onFinished(result);
                });
        }};

    return true;
}
} // namespace VideoDemo

#pragma once

#include <eacp/Core/Core.h>
#include <eacp/Network/HTTP/Http.h>

#include <thread>

namespace VideoDemo
{
// Fetches one file over HTTP on a worker thread. Writes to `<path>.part` and
// renames on success, so an interrupted transfer cannot leave a truncated file
// that later looks complete.
class Downloader
{
public:
    struct Result
    {
        bool ok = false;
        bool cancelled = false;

        std::string error;
        eacp::FilePath path;
    };

    Downloader() = default;
    ~Downloader();

    Downloader(const Downloader&) = delete;
    Downloader& operator=(const Downloader&) = delete;

    // `onFinished` runs on the main thread exactly once, unless this is
    // destroyed first. Returns false if a transfer is already in flight.
    bool start(std::string url,
               eacp::FilePath path,
               std::function<void(Result)> onFinished);

    // The completion still arrives, with Result::cancelled set.
    void cancel();

    bool isRunning() const { return running; }

    std::int64_t bytesReceived() const;

    // The size the server declared, or -1 when it declared none.
    std::int64_t totalBytes() const;

    // 0..1 through the transfer, or -1 when the total is unknown.
    float fraction() const;

private:
    void join();

    // Shared with the worker so it stays alive if a new transfer replaces it.
    std::shared_ptr<eacp::HTTP::DownloadProgress> progress =
        std::make_shared<eacp::HTTP::DownloadProgress>();

    std::shared_ptr<bool> alive = std::make_shared<bool>(true);
    std::thread worker;
    bool running = false;
};
} // namespace VideoDemo

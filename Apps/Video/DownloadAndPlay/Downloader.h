#pragma once

#include <eacp/Core/Core.h>
#include <eacp/Network/HTTP/Http.h>

#include <thread>

namespace VideoDemo
{
// Fetches one file over HTTP on a worker thread, with progress a UI can poll
// and a completion delivered back on the main thread.
//
// Two details this exists to keep out of the view:
//
//  * The transfer writes to `<path>.part` and is renamed into place only once
//    it has finished, so an interrupted download cannot leave a truncated file
//    that later looks like a complete one and gets used.
//
//  * The completion hop is fenced. The worker outlives nothing — the destructor
//    cancels and joins — but the callback it posts to the main thread can still
//    be sitting in the queue when this object goes away, so it is guarded by a
//    token rather than capturing a raw this.
//
// One transfer at a time; start() while running is refused.
class Downloader
{
public:
    struct Result
    {
        bool ok = false;
        bool cancelled = false;

        // Empty when ok. Either the transport's own message or the HTTP status.
        std::string error;

        // Where the finished file landed. Only valid when ok.
        eacp::FilePath path;
    };

    Downloader() = default;
    ~Downloader();

    Downloader(const Downloader&) = delete;
    Downloader& operator=(const Downloader&) = delete;

    // Begins fetching `url` into `path`. `onFinished` runs on the main thread,
    // exactly once, unless this Downloader is destroyed first. Returns false if
    // a transfer is already in flight.
    bool start(std::string url,
               eacp::FilePath path,
               std::function<void(Result)> onFinished);

    // Asks the transfer to stop. The completion still arrives, with
    // Result::cancelled set.
    void cancel();

    bool isRunning() const { return running; }

    std::int64_t bytesReceived() const;

    // The size the server declared, or -1 when it declared none.
    std::int64_t totalBytes() const;

    // 0..1 through the transfer, or -1 when the total is unknown.
    float fraction() const;

private:
    void join();

    // Shared with the worker so progress stays readable, and stays alive, even
    // if a new transfer replaces it.
    std::shared_ptr<eacp::HTTP::DownloadProgress> progress =
        std::make_shared<eacp::HTTP::DownloadProgress>();

    std::shared_ptr<bool> alive = std::make_shared<bool>(true);
    std::thread worker;
    bool running = false;
};
} // namespace VideoDemo

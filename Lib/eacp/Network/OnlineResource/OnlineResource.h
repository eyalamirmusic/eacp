#pragma once

#include "../Common.h"

#include <eacp/Core/Threads/Async.h>
#include <eacp/Core/Utils/FilePath.h>

namespace eacp
{
// A file an app needs from the network, kept on disk and fetched at most
// once: a fetch finds the local copy, asks the server whether it changed
// when the server gave it something to ask with, and downloads only when it
// has to. Each successful download leaves a sidecar (<path>.resource.json)
// recording the URL, the version and the server's validators; a later fetch
// reads it back to decide. A .zip is unpacked and path() is the folder.
//
// Three ways in, from the most to the least hands-on:
//
//  * An OnlineResource object: start() returns an Async, and the object is
//    where cancel() and progress() live. For a view that draws a progress
//    bar and offers a cancel button.
//  * fetchAsync(Options): the object is owned for you and the callbacks in
//    Options are called on the main thread as it goes.
//  * fetch(Options): the same, blocking until the resource is on disk. For a
//    console app that needs the file before it does anything else.
//
// The transfer runs on a worker thread. It writes to <path>.part and renames
// into place only once whole, so an interrupted download never passes for a
// complete one. Destroying the object cancels a transfer in flight and
// abandons its Async, so no continuation runs into a view that is gone.
class OnlineResource
{
public:
    struct Info
    {
        // For messages; the file name when empty. The file on disk is named
        // by fileName.
        std::string name;
        std::string url;

        // Empty takes the URL's last path segment. Ends in .zip to unpack.
        std::string fileName;

        // Bumping it forces a download regardless of what the server says.
        std::string version;
    };

    enum class Freshness
    {
        // With a local copy, one conditional GET (If-None-Match /
        // If-Modified-Since) when the server sent an ETag or Last-Modified
        // the last time; a 304 keeps the copy. A server that sent neither
        // is not asked again.
        check,

        // Never contact the server once a copy is present.
        trust
    };

    struct Result
    {
        bool ok = false;
        bool cancelled = false;

        // False when the local copy was kept, whether or not the server was
        // asked. Only meaningful when ok.
        bool downloaded = false;

        // Empty unless !ok && !cancelled: the transport's message or the
        // HTTP status.
        std::string error;

        // The file, or the unpacked directory for a zip. Valid when ok.
        FilePath path;
    };

    struct Progress
    {
        enum class Stage
        {
            idle,
            downloading,
            extracting,
            done
        };

        Stage stage = Stage::idle;
        std::int64_t bytesReceived = 0;

        // What the server declared, -1 when it declared nothing.
        std::int64_t totalBytes = -1;

        // 0..1 through the download, -1 while the total is unknown.
        float fraction = -1.0f;
    };

    // FilePath::appSupportDirectory() / "Resources"
    static FilePath defaultDirectory();

    // Everything the free functions need in one place: the resource, where
    // it goes, and what to call as it arrives. Both callbacks run on the
    // main thread; onProgress every progressInterval while a transfer is
    // under way and once more when it is over, onFinished exactly once.
    struct Options : Info
    {
        FilePath directory = defaultDirectory();
        Freshness freshness = Freshness::check;

        std::function<void(const Progress&)> onProgress = [](const Progress&) {};
        std::function<void(const Result&)> onFinished = [](const Result&) {};
        Time::MS progressInterval {100};

        // For the blocking fetch(): how long to wait before giving up, with
        // the result saying so and the transfer cancelled. Zero waits for
        // as long as it takes.
        Time::MS timeout {0};
    };

    // Runs the whole fetch with no object to hold: resolves on the main
    // thread, and calls the Options callbacks along the way.
    static Threads::Async<Result> fetchAsync(Options options);

    // fetchAsync, pumping the event loop until it is done. Main thread only,
    // and not from inside another event-loop callback; meant for the start
    // of a console app, before its own loop runs. Throws std::runtime_error
    // when the resource could not be had - a failed transfer, a cancel or
    // the timeout - so the caller uses the path without checking.
    static Result fetch(Options options);

    explicit OnlineResource(Info info,
                            FilePath directory = defaultDirectory(),
                            Freshness freshness = Freshness::check);
    ~OnlineResource();

    OnlineResource(const OnlineResource&) = delete;
    OnlineResource& operator=(const OnlineResource&) = delete;

    const Info& info() const { return resource; }
    const FilePath& directory() const { return folder; }

    // Where the resource is, or will be, on disk.
    FilePath path() const;

    // A complete copy of this URL and version is on disk. Local only:
    // start() may still ask the server whether it changed.
    bool isAvailable() const;

    // Resolves on the main thread, exactly once, unless this object is
    // destroyed first. Resolves at once with downloaded=false when the copy
    // is trusted. While one is running a second start() resolves !ok with
    // an error saying so.
    Threads::Async<Result> start();

    // Asks the transfer to stop; it still resolves, with cancelled set.
    void cancel();

    bool isRunning() const;

    // Readable from any thread, for a progress bar drawn while running.
    Progress progress() const;

    // Deletes the local copy and its sidecar. Not while running.
    bool remove();

private:
    struct Job;

    Info resource;
    FilePath folder;
    Freshness freshness;
    std::shared_ptr<Job> job;
};
} // namespace eacp

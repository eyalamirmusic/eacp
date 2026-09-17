#pragma once

#include "OnlineResource.h"

#include <functional>
#include <mutex>
#include <optional>

namespace eacp
{
// The process-wide picture of every online resource: where they go, what
// is on disk, what is being fetched and how far along it is, and what
// failed. Every OnlineResource reports here as it starts, finishes and
// removes, so a widget drawing this needs no handle to the object that
// owns a transfer - a view that fetched something and went away still
// leaves its outcome here. An app can also declare a resource before
// anything fetches it, so the same widget lists what it could fetch as
// well as what it has.
//
// The directory is the app's one place for resources: what
// OnlineResource::defaultDirectory() answers, where declare() puts an
// entry and what clear() empties. An OnlineResource told another directory
// still reports here, keyed by the path it lands at, and is simply not
// under the one clear() deletes.
//
// Every mutation and every listener call is on the main thread; entries()
// may be read from any thread and carries the live progress of a running
// transfer, read from the transfer's own atomics. State changes wake the
// listeners once per event-loop turn; progress does not, and a widget
// polls it with a timer while anything is fetching.
class OnlineResources
{
public:
    // What last happened to the entry. Separate from `available`, since a
    // failed or cancelled fetch leaves whatever copy was there before.
    enum class Status
    {
        idle,
        fetching,
        fetched,
        failed,
        cancelled
    };

    struct Entry
    {
        OnlineResource::Info info;
        FilePath directory;
        FilePath path;

        // A complete copy of this URL and version is on disk.
        bool available = false;

        // Of the file, or of every file under the unpacked folder. Zero
        // while not available.
        std::int64_t sizeOnDisk = 0;

        Status status = Status::idle;

        // Live while fetching; the last seen values afterwards.
        OnlineResource::Progress progress;

        // Set while status is failed.
        std::string error;

        bool isFetching() const { return status == Status::fetching; }
    };

    static OnlineResources& get();

    // FilePath::appSupportDirectory() / "Resources" until set. Setting it
    // moves nothing: entries already listed keep the path they were given.
    void setDirectory(FilePath newDirectory);
    FilePath getDirectory() const;

    // Lists a resource in the directory without fetching it. Idempotent: an
    // entry already known keeps its status, but its availability is re-read
    // from disk.
    void declare(OnlineResource::Info info);

    // Fetches a known entry, owning the object for the duration. Resolves
    // at once, not ok, for a path nobody declared or fetched before.
    Threads::Async<OnlineResource::Result> fetch(const FilePath& path);

    void cancel(const FilePath& path);

    // Deletes the entry's copy from disk; it stays listed as not available.
    // Refused while it is fetching.
    bool remove(const FilePath& path);

    // Drops the entry from the list. Nothing on disk changes.
    void forget(const FilePath& path);

    // Deletes the directory and everything in it. Refused while anything
    // under it is fetching; every entry under it is then listed as not
    // available.
    bool clear();

    bool isAnythingFetching() const;

    Vector<Entry> entries() const;
    std::optional<Entry> find(const FilePath& path) const;

    using ListenerId = int;
    ListenerId addListener(std::function<void()> listener);
    void removeListener(ListenerId id);

private:
    friend class OnlineResource;

    struct Record
    {
        Entry entry;
        std::function<OnlineResource::Progress()> liveProgress;
        std::function<void()> cancelLive;

        // The object behind fetch(), kept until the next fetch or forget.
        OwningPointer<OnlineResource> ownedResource;
    };

    struct Listener
    {
        ListenerId id = 0;
        std::function<void()> callback;
    };

    Record& recordFor(const OnlineResource& resource);
    Record* findRecord(const FilePath& path);
    void refreshFromDisk(Record& record);
    void notify();
    void deliverNotification();

    void reportStarted(const OnlineResource& resource,
                       std::function<OnlineResource::Progress()> liveProgress,
                       std::function<void()> cancelLive);
    void reportFinished(const FilePath& path, const OnlineResource::Result& result);
    void reportRemoved(const FilePath& path);

    mutable std::mutex mutex;
    FilePath directory = FilePath::appSupportDirectory() / "Resources";
    Vector<Record> records;
    Vector<Listener> listeners;
    ListenerId nextListenerId = 1;
    bool notificationPending = false;
};
} // namespace eacp

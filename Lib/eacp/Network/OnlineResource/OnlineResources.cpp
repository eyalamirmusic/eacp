#include "OnlineResources.h"

#include <eacp/Core/Threads/EventLoop.h>
#include <eacp/Core/Utils/File.h>
#include <eacp/Core/Utils/StdPath.h>

#include <filesystem>

namespace eacp
{
namespace
{
std::int64_t sizeOnDisk(const FilePath& path)
{
    auto file = File {path};

    if (file.isRegularFile())
        return (std::int64_t) file.size();

    auto total = std::int64_t {0};
    auto ignored = std::error_code {};

    for (const auto& entry:
         std::filesystem::recursive_directory_iterator(toStdPath(path), ignored))
        if (entry.is_regular_file(ignored))
            total += (std::int64_t) entry.file_size(ignored);

    return total;
}

bool isUnder(const FilePath& path, const FilePath& root)
{
    return path == root || File {path}.isUnder(root);
}
} // namespace

OnlineResources& OnlineResources::get()
{
    static OnlineResources instance;
    return instance;
}

OnlineResources::Record* OnlineResources::findRecord(const FilePath& path)
{
    for (auto& record: records)
        if (record.entry.path == path)
            return &record;

    return nullptr;
}

OnlineResources::Record& OnlineResources::recordFor(const OnlineResource& resource)
{
    if (auto* existing = findRecord(resource.path()))
    {
        existing->entry.info = resource.info();
        return *existing;
    }

    auto& record = records.add(Record {});
    record.entry.info = resource.info();
    record.entry.directory = resource.directory();
    record.entry.path = resource.path();
    return record;
}

void OnlineResources::refreshFromDisk(Record& record)
{
    auto& entry = record.entry;
    entry.available = OnlineResource {entry.info, entry.directory}.isAvailable();
    entry.sizeOnDisk = entry.available ? sizeOnDisk(entry.path) : 0;
}

void OnlineResources::setDirectory(FilePath newDirectory)
{
    {
        auto lock = std::scoped_lock {mutex};
        directory = std::move(newDirectory);
    }

    notify();
}

FilePath OnlineResources::getDirectory() const
{
    auto lock = std::scoped_lock {mutex};
    return directory;
}

void OnlineResources::declare(OnlineResource::Info info)
{
    auto resource = OnlineResource {std::move(info), getDirectory()};

    {
        auto lock = std::scoped_lock {mutex};
        refreshFromDisk(recordFor(resource));
    }

    notify();
}

Threads::Async<OnlineResource::Result> OnlineResources::fetch(const FilePath& path)
{
    auto* resource = static_cast<OnlineResource*>(nullptr);

    {
        auto lock = std::scoped_lock {mutex};

        if (auto* record = findRecord(path); record && !record->entry.isFetching())
        {
            record->ownedResource.create(record->entry.info,
                                         record->entry.directory);
            resource = record->ownedResource.get();
        }
    }

    if (resource != nullptr)
        return resource->start();

    auto refused = Threads::AsyncPromise<OnlineResource::Result> {};
    auto result = OnlineResource::Result {};
    result.error = path.str() + " is not a known resource, or is being fetched";
    result.path = path;
    refused.resolve(std::move(result));
    return refused.get();
}

void OnlineResources::cancel(const FilePath& path)
{
    auto cancelLive = std::function<void()> {};

    {
        auto lock = std::scoped_lock {mutex};

        if (auto* record = findRecord(path); record && record->cancelLive)
            cancelLive = record->cancelLive;
    }

    if (cancelLive)
        cancelLive();
}

bool OnlineResources::remove(const FilePath& path)
{
    auto entry = find(path);

    if (!entry || entry->isFetching())
        return false;

    return OnlineResource {entry->info, entry->directory}.remove();
}

void OnlineResources::forget(const FilePath& path)
{
    {
        auto lock = std::scoped_lock {mutex};
        records.eraseIf([&](const Record& record)
                        { return record.entry.path == path; });
    }

    notify();
}

bool OnlineResources::isAnythingFetching() const
{
    auto lock = std::scoped_lock {mutex};

    for (const auto& record: records)
        if (record.entry.isFetching() && isUnder(record.entry.directory, directory))
            return true;

    return false;
}

bool OnlineResources::clear()
{
    if (isAnythingFetching())
        return false;

    auto folder = getDirectory();
    auto ignored = std::error_code {};
    std::filesystem::remove_all(toStdPath(folder), ignored);

    {
        auto lock = std::scoped_lock {mutex};

        for (auto& record: records)
        {
            if (!isUnder(record.entry.directory, folder))
                continue;

            record.entry.available = false;
            record.entry.sizeOnDisk = 0;
        }
    }

    notify();
    return !File {folder}.exists();
}

Vector<OnlineResources::Entry> OnlineResources::entries() const
{
    auto lock = std::scoped_lock {mutex};
    auto result = Vector<Entry> {};

    for (const auto& record: records)
    {
        auto& entry = result.add(record.entry);

        if (record.liveProgress)
            entry.progress = record.liveProgress();
    }

    return result;
}

std::optional<OnlineResources::Entry>
    OnlineResources::find(const FilePath& path) const
{
    for (auto& entry: entries())
        if (entry.path == path)
            return entry;

    return std::nullopt;
}

OnlineResources::ListenerId
    OnlineResources::addListener(std::function<void()> listener)
{
    auto lock = std::scoped_lock {mutex};
    auto id = nextListenerId++;
    listeners.add({id, std::move(listener)});
    return id;
}

void OnlineResources::removeListener(ListenerId id)
{
    auto lock = std::scoped_lock {mutex};
    listeners.eraseIf([id](const Listener& listener) { return listener.id == id; });
}

// One wake-up per event-loop turn however many changes preceded it, and
// never from inside the call that changed something, so a listener is free
// to ask the registry anything.
void OnlineResources::notify()
{
    {
        auto lock = std::scoped_lock {mutex};

        if (notificationPending)
            return;

        notificationPending = true;
    }

    Threads::callAsync([this] { deliverNotification(); });
}

void OnlineResources::deliverNotification()
{
    auto callbacks = Vector<std::function<void()>> {};

    {
        auto lock = std::scoped_lock {mutex};
        notificationPending = false;

        for (const auto& listener: listeners)
            callbacks.add(listener.callback);
    }

    for (const auto& callback: callbacks)
        callback();
}

void OnlineResources::reportStarted(
    const OnlineResource& resource,
    std::function<OnlineResource::Progress()> liveProgress,
    std::function<void()> cancelLive)
{
    {
        auto lock = std::scoped_lock {mutex};
        auto& record = recordFor(resource);
        record.entry.status = Status::fetching;
        record.entry.error.clear();
        record.entry.progress = liveProgress();
        record.liveProgress = std::move(liveProgress);
        record.cancelLive = std::move(cancelLive);
    }

    notify();
}

void OnlineResources::reportFinished(const FilePath& path,
                                     const OnlineResource::Result& result)
{
    {
        auto lock = std::scoped_lock {mutex};
        auto* record = findRecord(path);

        if (record == nullptr)
            return;

        if (record->liveProgress)
            record->entry.progress = record->liveProgress();

        record->liveProgress = nullptr;
        record->cancelLive = nullptr;

        if (result.ok)
            record->entry.status = Status::fetched;
        else if (result.cancelled)
            record->entry.status = Status::cancelled;
        else
            record->entry.status = Status::failed;

        record->entry.error = result.error;
        refreshFromDisk(*record);
    }

    notify();
}

void OnlineResources::reportRemoved(const FilePath& path)
{
    {
        auto lock = std::scoped_lock {mutex};

        if (auto* record = findRecord(path))
        {
            record->entry.available = false;
            record->entry.sizeOnDisk = 0;
        }
    }

    notify();
}
} // namespace eacp

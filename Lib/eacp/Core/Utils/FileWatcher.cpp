#include "FileWatcher.h"
#include "File.h"
#include "Files.h"
#include "../Threads/Timer.h"

#include <map>
#include <optional>
#include <set>

namespace eacp::Files
{
namespace
{
struct FileSignature
{
    std::int64_t modificationTime = 0;
    std::uint64_t size = 0;

    bool operator==(const FileSignature&) const = default;
};

using Snapshot = std::map<std::string, FileSignature>;

FileSignature signatureOf(const FilePath& path)
{
    auto file = File {path};
    return {file.modificationTime(), file.size()};
}

DirectoryOptions directoryOptionsFor(const FileWatcherOptions& options)
{
    auto dirOptions = DirectoryOptions {};
    dirOptions.recursive = options.recursive;
    dirOptions.includeHidden = options.includeHidden;
    dirOptions.pattern = options.pattern;
    return dirOptions;
}

std::string_view withoutTrailingSlash(const std::string& path)
{
    auto view = std::string_view {path};

    while (view.size() > 1 && view.back() == '/')
        view.remove_suffix(1);

    return view;
}

bool isExcluded(const FilePath& directory, const Vector<FilePath>& excluded)
{
    auto name = withoutTrailingSlash(directory.str());

    for (const auto& candidate: excluded)
        if (withoutTrailingSlash(candidate.str()) == name)
            return true;

    return false;
}

void addDirectoryFiles(Snapshot& snapshot,
                       const FilePath& directory,
                       const DirectoryOptions& dirOptions,
                       const Vector<FilePath>& excluded)
{
    auto visit = [&](const DirectoryEntry& entry)
    {
        if (entry.kind == EntryKind::directory && isExcluded(entry.path, excluded))
            return Visit::skipChildren;

        if (entry.kind == EntryKind::file)
            snapshot[entry.path.str()] = signatureOf(entry.path);

        return Visit::next;
    };

    forEachEntry(directory, dirOptions, visit);
}

Snapshot takeSnapshot(const Vector<FilePath>& paths,
                      const DirectoryOptions& dirOptions,
                      const Vector<FilePath>& excluded)
{
    auto snapshot = Snapshot {};

    for (const auto& path: paths)
    {
        if (isDirectory(path))
        {
            addDirectoryFiles(snapshot, path, dirOptions, excluded);
        }
        else if (isFile(path))
        {
            snapshot[path.str()] = signatureOf(path);
        }
    }

    return snapshot;
}

void addDifferences(const Snapshot& before,
                    const Snapshot& after,
                    std::set<std::string>& changed)
{
    for (const auto& [path, signature]: after)
    {
        auto previous = before.find(path);

        if (previous == before.end() || !(previous->second == signature))
            changed.insert(path);
    }

    for (const auto& [path, signature]: before)
    {
        if (!after.contains(path))
            changed.insert(path);
    }
}

Vector<FilePath> toPaths(const std::set<std::string>& names)
{
    auto paths = Vector<FilePath> {};
    paths.reserve(names.size());

    for (const auto& name: names)
        paths.add(FilePath {name});

    return paths;
}
} // namespace

struct FileWatcher::Impl
{
    Impl(const Vector<FilePath>& pathsToUse,
         const ChangedFunc& onChangedToUse,
         const FileWatcherOptions& optionsToUse)
        : paths(pathsToUse)
        , onChanged(onChangedToUse)
        , options(optionsToUse)
        , dirOptions(directoryOptionsFor(optionsToUse))
        , snapshot(takeSnapshot(paths, dirOptions, options.excludedDirectories))
    {
        auto tick = [this] { poll(); };
        timer.emplace(tick, options.pollInterval);
    }

    void poll()
    {
        auto current = takeSnapshot(paths, dirOptions, options.excludedDirectories);

        if (current != snapshot)
        {
            addDifferences(snapshot, current, dirty);
            snapshot = current;
            settleDeadline.emplace(options.settle);
            return;
        }

        if (hasSettled())
            fire();
    }

    bool hasSettled() const
    {
        return !dirty.empty() && settleDeadline && settleDeadline->expired();
    }

    void fire()
    {
        auto changed = toPaths(dirty);
        dirty.clear();
        settleDeadline.reset();
        onChanged(changed);
    }

    Vector<FilePath> paths;
    ChangedFunc onChanged = [](const Vector<FilePath>&) {};
    FileWatcherOptions options;
    DirectoryOptions dirOptions;
    Snapshot snapshot;
    std::set<std::string> dirty;
    std::optional<Time::Deadline> settleDeadline;
    std::optional<Threads::Timer> timer;
};

FileWatcher::FileWatcher(const Vector<FilePath>& paths,
                         const ChangedFunc& onChanged,
                         const FileWatcherOptions& options)
    : impl(paths, onChanged, options)
{
}

FileWatcher::~FileWatcher()
{
    impl->timer.reset();
}

const Vector<FilePath>& FileWatcher::getPaths() const
{
    return impl->paths;
}
} // namespace eacp::Files

#include "SourceWatcher.h"

namespace eacp::Files
{
namespace
{
FileWatcherOptions fileWatcherOptionsFor(const SourceWatcher::Options& options)
{
    auto watcherOptions = options.watcher;
    watcherOptions.recursive = true;
    watcherOptions.pattern = options.pattern;
    return watcherOptions;
}

SourceWatcher::Options sourceWatcherOptionsFor(const FilePath& directory)
{
    auto options = SourceWatcher::Options {};
    options.directory = directory;
    return options;
}

Vector<FilePath> watchedPathsFor(const SourceWatcher::Options& options)
{
    if (options.files.empty())
        return {options.directory};

    return options.files;
}
} // namespace

SourceWatcher::SourceWatcher(const Options& options,
                             const FileWatcher::ChangedFunc& onChanged)
    : directory(options.directory)
    , watcher(watchedPathsFor(options), onChanged, fileWatcherOptionsFor(options))
{
}

SourceWatcher::SourceWatcher(const FilePath& directoryToWatch,
                             const FileWatcher::ChangedFunc& onChanged)
    : SourceWatcher(sourceWatcherOptionsFor(directoryToWatch), onChanged)
{
}

const FilePath& SourceWatcher::getDirectory() const
{
    return directory;
}

const Vector<FilePath>& SourceWatcher::getWatchedPaths() const
{
    return watcher.getPaths();
}
} // namespace eacp::Files

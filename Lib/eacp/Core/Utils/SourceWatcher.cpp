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
} // namespace

SourceWatcher::SourceWatcher(const Options& options,
                             const FileWatcher::ChangedFunc& onChanged)
    : watcher({options.directory}, onChanged, fileWatcherOptionsFor(options))
{
}

SourceWatcher::SourceWatcher(const FilePath& directoryToWatch,
                             const FileWatcher::ChangedFunc& onChanged)
    : SourceWatcher(sourceWatcherOptionsFor(directoryToWatch), onChanged)
{
}

const FilePath& SourceWatcher::getDirectory() const
{
    return watcher.getPaths()[0];
}
} // namespace eacp::Files

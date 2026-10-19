#pragma once

#include "Common.h"
#include "FilePath.h"

namespace eacp::Files
{
struct FileWatcherOptions
{
    Time::MS pollInterval {250};

    // How long a poll must find nothing new before the accumulated changes
    // are reported, so a burst of saves or a write-then-rename is one call.
    Time::MS settle {150};

    bool recursive = true;

    // A matchesWildcard pattern that files in a watched directory must match.
    // Empty watches every file.
    std::string pattern;
    bool includeHidden = false;
};

// Polls files and directories on the main thread and reports what was
// modified, added or removed since the last report, once a burst of changes
// has settled. A path that does not exist yet is watched for appearing.
//
// The callback runs on the main thread and must not destroy the watcher.
class FileWatcher
{
public:
    using ChangedFunc = std::function<void(const Vector<FilePath>& changed)>;

    FileWatcher(const Vector<FilePath>& paths,
                const ChangedFunc& onChanged,
                const FileWatcherOptions& options = {});
    ~FileWatcher();

    FileWatcher(const FileWatcher&) = delete;
    FileWatcher& operator=(const FileWatcher&) = delete;
    FileWatcher(FileWatcher&&) = delete;
    FileWatcher& operator=(FileWatcher&&) = delete;

    const Vector<FilePath>& getPaths() const;

private:
    struct Impl;
    Pimpl<Impl> impl;
};
} // namespace eacp::Files

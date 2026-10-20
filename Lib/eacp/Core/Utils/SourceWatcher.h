#pragma once

#include "FileWatcher.h"

namespace eacp::Files
{
inline constexpr auto sourceFilePattern =
    "*.cpp;*.cc;*.cxx;*.c;*.h;*.hpp;*.hh;*.inl;*.ipp;*.mm;*.m";

// A FileWatcher over a source tree: every file under the directory matching
// the pattern, recursively, outside watcher.excludedDirectories. A directory
// that does not exist yet is watched for appearing. The callback runs on the
// main thread, once a burst of changes has settled, and must not destroy the
// watcher.
class SourceWatcher
{
public:
    struct Options
    {
        FilePath directory;
        std::string pattern = sourceFilePattern;
        FileWatcherOptions watcher;
    };

    SourceWatcher(const Options& options, const FileWatcher::ChangedFunc& onChanged);
    SourceWatcher(const FilePath& directory,
                  const FileWatcher::ChangedFunc& onChanged);

    const FilePath& getDirectory() const;

private:
    FileWatcher watcher;
};
} // namespace eacp::Files

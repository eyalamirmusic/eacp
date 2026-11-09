#pragma once

#include "FileWatcher.h"

namespace eacp::Files
{
inline constexpr auto sourceFilePattern =
    "*.cpp;*.cc;*.cxx;*.c;*.h;*.hpp;*.hh;*.inl;*.ipp;*.mm;*.m";

// A FileWatcher over a source tree: every file under the directory matching
// the pattern, recursively, outside watcher.excludedDirectories. A directory
// that does not exist yet is watched for appearing. With `files`, exactly those
// files are watched instead, and the directory only names the tree they belong
// to. The callback runs on the main thread, once a burst of changes has
// settled, and must not destroy the watcher.
class SourceWatcher
{
public:
    struct Options
    {
        FilePath directory;
        std::string pattern = sourceFilePattern;
        FileWatcherOptions watcher;
        Vector<FilePath> files;
    };

    SourceWatcher(const Options& options, const FileWatcher::ChangedFunc& onChanged);
    SourceWatcher(const FilePath& directory,
                  const FileWatcher::ChangedFunc& onChanged);

    const FilePath& getDirectory() const;

    // The directory, or the files when Options::files named some.
    const Vector<FilePath>& getWatchedPaths() const;

private:
    FilePath directory;
    FileWatcher watcher;
};
} // namespace eacp::Files

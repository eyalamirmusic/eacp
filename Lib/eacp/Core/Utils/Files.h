#pragma once

#include "Common.h"
#include "File.h"
#include "FilePath.h"

namespace eacp::Files
{
std::string readFile(const FilePath& path);

// Writes bytes to path, creating parent directories first. Throws
// std::runtime_error when the file can't be opened or fully written.
void writeFile(const FilePath& path, Span<const std::uint8_t> bytes);

// Writes bytes so a concurrent reader sees either the whole old file or the
// whole new one, never a half-written mix: the data goes to a temporary
// sibling and is then renamed over the target, which the filesystem does
// atomically. Losing power or running out of disk part-way leaves the
// original untouched, which plain writeFile — opening the target with trunc —
// cannot promise.
//
// Two details a bare rename would get wrong, both of which lose information
// that was on the file before the save:
//
// - Symlinks are followed, so writing through one replaces what it points at
//   rather than turning the link into a regular file.
// - An existing file's permission bits are carried over, so saving a script
//   does not silently un-execute it by handing the replacement the umask.
//
// Throws std::runtime_error, like writeFile, if the write or the rename fails.
void writeFileAtomically(const FilePath& path, Span<const std::uint8_t> bytes);

// The directory and every missing parent. True when it exists afterwards.
bool createDirectories(const FilePath& directory);

// The file, or the directory and everything under it. True when nothing is
// left at path, which includes there having been nothing there to begin with.
bool removeAll(const FilePath& path);

enum class EntryKind
{
    file,
    directory,
    symlink,
    other
};

// What a traversal reports. `kind` is the entry itself, so a symlink is a
// symlink whatever it points at; file() follows it for size and modification
// time. `isHidden` is a leading '.' anywhere, and the hidden attribute as well
// on Windows.
struct DirectoryEntry
{
    FilePath path;
    EntryKind kind = EntryKind::other;

    // 0 for the directory's own children, 1 for theirs, and so on.
    int depth = 0;
    bool isHidden = false;

    File file() const;
};

struct TraversalError
{
    FilePath path;
    std::string message;
};

// What a visitor answers: carry on, carry on but do not descend into this
// directory, or end the traversal here.
enum class Visit
{
    next,
    skipChildren,
    stop
};

// Whether a symlink to a directory is descended. `follow` keeps the canonical
// path of every directory it enters and silently skips one it has already
// been in, so a link back up the tree is a dead end rather than a loop.
enum class Symlinks
{
    skip,
    follow
};

using VisitingFunc = std::function<Visit(const DirectoryEntry&)>;
using TraversalErrorFunc = std::function<Visit(const TraversalError&)>;

struct DirectoryOptions
{
    bool recursive = false;
    bool includeHidden = false;
    Symlinks symlinks = Symlinks::skip;

    // An entry or a directory that could not be read. The default skips it
    // and carries on; answer Visit::stop to end the traversal instead.
    TraversalErrorFunc onError = [](const TraversalError&) { return Visit::next; };
};

// Calls visitor once per entry and collects nothing, so a search can end at
// its first hit. Each directory's children come in name order and a
// directory's contents follow it directly, so a walk is deterministic. False
// when `directory` itself could not be read, which reaches onError first.
bool forEachEntry(const FilePath& directory,
                  const DirectoryOptions& options,
                  const VisitingFunc& visitor);

// forEachEntry collected, in the same order. Unreadable entries are skipped
// unless options.onError says otherwise.
Vector<DirectoryEntry> listDirectory(const FilePath& directory,
                                     const DirectoryOptions& options = {});

// The regular files alone: preset discovery. A symlink to a regular file
// counts under Symlinks::follow and not under Symlinks::skip.
Vector<FilePath> listFiles(const FilePath& directory,
                           const DirectoryOptions& options = {});

// The running process's executable. Empty if unresolvable.
FilePath executablePath();

// Where the running binary's shipped resources live: Contents/Resources for a
// bundle, the executable's own directory otherwise. Empty if unresolvable.
FilePath resourcesDirectory();

// The full path of a named resource, or empty when there is no such file.
std::string getBundleResourcePath(const std::string& filename);

std::string filenameFromPath(const std::string& path);
} // namespace eacp::Files

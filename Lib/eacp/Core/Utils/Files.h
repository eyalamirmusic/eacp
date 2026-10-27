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
void writeFile(const FilePath& path, std::string_view text);

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

// What is at path, following symlinks. False on any error.
bool exists(const FilePath& path);
bool isFile(const FilePath& path);
bool isDirectory(const FilePath& path);

// One regular file to `to`, replacing what is there and creating its parent
// directories. False when `from` is missing or is a directory.
bool copyFile(const FilePath& from, const FilePath& to);

// `from` and everything under it, as the directory `to` (not into it). An
// existing `to` is removed first, so the result is an exact copy; symlinks
// are copied as symlinks.
bool copyDirectory(const FilePath& from, const FilePath& to);

// Renames a file or directory, replacing what is at `to` and creating its
// parent directories. Across filesystems, where a rename cannot work, it
// copies and then removes the original.
bool moveFile(const FilePath& from, const FilePath& to);

// One file or symlink. False when path is a directory or there is nothing
// there; removeAll is the recursive, idempotent one.
bool removeFile(const FilePath& path);

// Adds bytes or text to the end of the file, creating it (and its parent
// directories) if it is missing.
bool appendFile(const FilePath& path, Span<const std::uint8_t> bytes);
bool appendFile(const FilePath& path, std::string_view text);

// Writes bytes at offset without truncating: what lies beyond is kept, and a
// write past the end extends the file (any gap reads as zeros). Creates the
// file if it is missing.
bool writeFileAt(const FilePath& path,
                 std::uint64_t offset,
                 Span<const std::uint8_t> bytes);

// name with the characters no filesystem accepts in a file name removed
// (" # @ , ; : < > * ^ | ? \ /) and cut to 128 characters, keeping a short
// extension. The same result as JUCE's File::createLegalFileName.
std::string legalFileName(std::string_view name);

// path itself when nothing is there, otherwise the first free "name(2).ext",
// "name(3).ext", ... beside it. A name already ending in "(n)" counts on
// from n.
FilePath nonexistentSibling(const FilePath& path);

// directory / (prefix + suffix) when that is free, otherwise the first free
// prefix(2)suffix, prefix(3)suffix, ...
FilePath nonexistentChild(const FilePath& directory,
                          std::string_view prefix,
                          std::string_view suffix);

// True when name matches one of the patterns in pattern, which are separated
// by ';' or ',' and trimmed, so "*.wav; *.flac" works. '*' matches any run of
// characters and '?' any one; case is ignored. An empty pattern matches
// everything.
bool matchesWildcard(std::string_view name, std::string_view pattern);

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

    // A matchesWildcard pattern ("*.wav; *.flac") that files must match to be
    // visited. Directories are visited and descended whatever their name.
    // Empty visits everything.
    std::string pattern;

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

// Hands path to the system to open as if the user had double-clicked it: a
// directory opens in the file manager (Finder, Explorer, the desktop's own),
// a file in its default application. JUCE's File::startAsProcess() with no
// parameters.
//
// True when the open was handed off: NSWorkspace openURL: on macOS,
// ShellExecuteW with the default verb on Windows, a spawned xdg-open on
// Linux. On Linux that only means xdg-open started; a desktop with no handler
// for the type is xdg-open's to report. False when path is empty or nothing
// is there, when the launch failed, and always on iOS and Android, where an
// app has no file manager to hand its own files to. A relative path is
// resolved against the working directory first.
//
// Returns once the launch is handed off, never waiting for the application
// it starts, and never throws, so it is safe from the message thread.
bool openWithSystem(const FilePath& path);

// Shows path in the file manager with it selected, as JUCE's
// File::revealToUser() does, for a file or a directory alike: Finder through
// NSWorkspace selectFile:inFileViewerRootedAtPath: on macOS, Explorer
// through SHOpenFolderAndSelectItems on Windows (falling back to
// "explorer /select," when that fails). Linux has no portable way to select
// an item, so it opens revealFallbackDirectory(path) with xdg-open instead.
// The result, the threading and the iOS and Android answer are
// openWithSystem's.
bool revealInFileManager(const FilePath& path);

// The folder a file manager that cannot select an item opens to show it: the
// parent of path, ignoring a trailing separator, or path itself when it has
// no parent (a root such as "/" or "C:/"). Pure text: nothing is checked on
// disk.
FilePath revealFallbackDirectory(const FilePath& path);

// The running process's executable. Empty if unresolvable.
FilePath executablePath();

// What a user sees as the application holding this code, resolved from the
// code's own module (so a plugin answers for itself, not its host): the
// enclosing .app (or .vst3, .component) bundle on macOS, the bundle directory
// on iOS, the executable or library file elsewhere. Matches JUCE's
// File::currentApplicationFile.
FilePath applicationPath();

// The bundle a binary at "<bundle>/Contents/MacOS/<name>" belongs to, e.g.
// "/Applications/Foo.app" for "/Applications/Foo.app/Contents/MacOS/Foo"
// (case ignored, as on a default macOS volume). Empty for any other path.
// Pure text: nothing is checked on disk.
FilePath enclosingBundle(const FilePath& binary);

// Where the running binary's shipped resources live: Contents/Resources for a
// bundle, the executable's own directory otherwise. Empty if unresolvable.
FilePath resourcesDirectory();

// The full path of a named resource, or empty when there is no such file.
std::string getBundleResourcePath(const std::string& filename);

std::string filenameFromPath(const std::string& path);
} // namespace eacp::Files

#pragma once

#include "Common.h"
#include "FilePath.h"

#include <span>

namespace eacp
{
// RAII handle for reading a file off disk in bounded chunks, without
// loading the whole thing into memory.
//
// Non-copyable (it owns an open stream); move it or hold it behind a
// shared_ptr when a reader closure must outlive the call that made it.
class File
{
public:
    explicit File(FilePath path);

    const FilePath& path() const { return filePath; }

    bool exists() const;
    bool isRegularFile() const;

    // True if this file's path resolves inside `root` (no escape via "..",
    // symlinks, etc.). Use it to sandbox files served to untrusted callers.
    bool isUnder(const FilePath& root) const;

    // Size in bytes, or 0 if the file is missing or not a regular file.
    std::uint64_t size() const;

    // Opens the file for binary reading. Returns false if it can't be
    // opened. A no-op once already open.
    bool openForRead();

    bool isOpen() const;

    // Reads up to `out.size()` bytes starting at byte `offset`, returning the
    // number actually read (0 at end of file). Opens the file on first use,
    // and only seeks when `offset` differs from the current position, so
    // sequential reads stay cheap.
    std::size_t read(std::uint64_t offset, std::span<std::uint8_t> out);

private:
    struct Impl;

    FilePath filePath;
    Pimpl<Impl> impl;
};
} // namespace eacp

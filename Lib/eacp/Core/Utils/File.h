#pragma once

#include "Common.h"
#include "FilePath.h"

#include <span>

namespace eacp
{
// RAII handle for reading a file in bounded chunks. Non-copyable because it
// owns an open stream; move it, or hold it behind a shared_ptr.
class File
{
public:
    explicit File(FilePath path);

    const FilePath& path() const { return filePath; }

    bool exists() const;
    bool isRegularFile() const;

    // True if the path resolves inside `root`, with no escape via ".." or a
    // symlink. Use it to sandbox files served to untrusted callers.
    bool isUnder(const FilePath& root) const;

    // Size in bytes, or 0 if the file is missing or not a regular file.
    std::uint64_t size() const;

    // Comparable only against itself: std::filesystem's clock has an
    // unspecified epoch, so this is not a wall-clock time. 0 when missing. Pair
    // it with size(), since two quick writes can share a timestamp.
    std::int64_t modificationTime() const;

    // False if it can't be opened. A no-op once already open.
    bool openForRead();

    bool isOpen() const;

    // Returns the number of bytes actually read, 0 at end of file. Opens on
    // first use, and only seeks when `offset` differs from the current position.
    std::size_t read(std::uint64_t offset, std::span<std::uint8_t> out);

private:
    struct Impl;

    FilePath filePath;
    Pimpl<Impl> impl;
};
} // namespace eacp

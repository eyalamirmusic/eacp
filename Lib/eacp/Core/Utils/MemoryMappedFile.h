#pragma once

#include "Common.h"
#include "FilePath.h"

#include <span>

namespace eacp
{
// A read-only view of a file's bytes, placed in the address space rather than
// copied into it: no allocation, no read, and pages arrive from the page cache
// as they are touched. Suits work that walks something large — decoding an
// image, hashing, scanning, serving byte ranges — where readFile would cost a
// copy of the whole file before the first byte can be looked at.
//
// Non-copyable (it owns a mapping); move it, or hold it behind a shared_ptr
// when a reader closure must outlive the call that made it.
//
// The saved copy is paid for with a live dependency on the file, and it is
// worth knowing which one you are taking on. The bytes stay the file's, so
// another process truncating it pulls the ground out from under pages already
// handed out: touching them then raises SIGBUS on POSIX, which portable code
// cannot catch. Windows does not have that hole — an open mapping makes the
// truncation itself fail — so this is a POSIX-only hazard, and one that only
// arises for files something else is free to shorten. Map what you control;
// read what you don't.
class MemoryMappedFile
{
public:
    // A length meaning "from offset to the end of the file".
    static constexpr std::uint64_t toEndOfFile = ~std::uint64_t {0};

    // Maps length bytes starting at offset, or the whole file by default.
    // offset is a plain byte offset — the page and 64KB-granularity alignment
    // the platforms insist on is handled internally — and a length running
    // past the end is clamped to what the file holds.
    //
    // Never throws: ask isValid(). It is false when the file is missing, is
    // not a regular file, cannot be read, or offset is past its end.
    explicit MemoryMappedFile(const FilePath& path,
                              std::uint64_t offset = 0,
                              std::uint64_t length = toEndOfFile);

    const FilePath& path() const { return filePath; }

    bool isValid() const;

    // Valid and empty are independent. Mapping an empty file, or the zero
    // bytes at the very end of one, succeeds and yields an empty span: both
    // platforms reject a zero-length mapping, so that case is answered here
    // rather than turned into a failure every caller has to special-case.
    std::span<const std::uint8_t> bytes() const;
    std::string_view text() const;

    std::size_t size() const;
    bool empty() const;

private:
    struct Native;

    FilePath filePath;
    Pimpl<Native> impl;
};
} // namespace eacp

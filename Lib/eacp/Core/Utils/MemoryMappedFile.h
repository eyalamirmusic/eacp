#pragma once

#include "Common.h"
#include "FilePath.h"

#include <span>

namespace eacp
{
// A read-only view of a file's bytes, non-copyable because it owns the mapping.
// The bytes stay the file's: on POSIX, another process truncating it makes
// touching mapped pages raise an uncatchable SIGBUS. Map only what you control.
class MemoryMappedFile
{
public:
    static constexpr std::uint64_t toEndOfFile = ~std::uint64_t {0};

    // `offset` is a plain byte offset (alignment is handled internally) and an
    // over-long `length` is clamped. Never throws: ask isValid(), which is false
    // for a missing or non-regular file, or an offset past its end.
    explicit MemoryMappedFile(const FilePath& path,
                              std::uint64_t offset = 0,
                              std::uint64_t length = toEndOfFile);

    const FilePath& path() const { return filePath; }

    bool isValid() const;

    // Valid and empty are independent: mapping an empty file succeeds and
    // yields an empty span, though both platforms reject a zero-length mapping.
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

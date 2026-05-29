#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <span>

namespace eacp
{
// RAII handle for reading a file off disk in bounded chunks, without ever
// loading the whole thing into memory. Wraps std::filesystem queries and a
// lazily-opened std::ifstream behind a small, range-friendly API.
//
// Non-copyable (it owns a stream); move it or keep it behind a shared_ptr
// when a reader closure needs to outlive the call that created it.
class File
{
public:
    explicit File(std::filesystem::path path);

    const std::filesystem::path& path() const { return filePath; }

    bool exists() const;
    bool isRegularFile() const;

    // True if this file's path resolves to a location inside `root` (not an
    // escape via "..", symlinks, etc.). The guard for serving files off disk
    // to untrusted callers: reject anything outside an allowed root.
    bool isUnder(const std::filesystem::path& root) const;

    // Size in bytes, or 0 if the file is missing or not a regular file.
    std::uint64_t size() const;

    // Opens the file for binary reading. Returns false if it can't be
    // opened. A no-op once already open.
    bool openForRead();

    bool isOpen() const { return stream.is_open(); }

    // Reads up to `out.size()` bytes starting at byte `offset`, returning the
    // number actually read (0 at end of file). Opens the file on first use,
    // and only seeks when `offset` differs from the current position, so
    // sequential reads stay cheap.
    std::size_t read(std::uint64_t offset, std::span<std::uint8_t> out);

private:
    std::filesystem::path filePath;
    std::ifstream stream;
    std::uint64_t position = 0;
};
} // namespace eacp

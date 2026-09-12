#pragma once

#include "Common.h"
#include "FilePath.h"

// Zip archives and raw deflate, over a vendored miniz (ThirdParty/miniz) that
// nothing outside Zip.cpp sees. Archives are read from a file or from bytes
// already in memory and written to either, and single blobs can be deflated
// on their own for a cache entry or a wire format that carries no archive.
namespace eacp::Zip
{
// An archive or an entry can outgrow an int, so count one with getSize().
using Bytes = Vector<std::uint8_t>;

enum class Level
{
    none,
    fastest,
    normal,
    best
};

struct Entry
{
    // As stored, '/' separated, with a trailing '/' on a directory.
    std::string name;
    std::uint64_t size = 0;
    std::uint64_t compressedSize = 0;

    // Seconds since the Unix epoch, 0 when the archive stores none.
    std::int64_t modificationTime = 0;
    bool isDirectory = false;
};

// Reads an existing archive. Never throws: ask isValid(). A file is mapped
// rather than copied, so the same hazard MemoryMappedFile documents applies -
// read what something else may truncate through the Bytes constructor.
//
// Non-copyable (it owns the mapping or the bytes); move it.
class Reader
{
public:
    explicit Reader(const FilePath& path);
    explicit Reader(Bytes bytes);

    bool isValid() const;

    // Why the last operation failed, empty when it did not.
    std::string errorMessage() const;

    int numEntries() const;
    Vector<Entry> entries() const;

    // Names match exactly, case included.
    bool contains(std::string_view name) const;
    std::optional<Entry> find(std::string_view name) const;

    // The whole entry, inflated. Nullopt when there is no such entry, it is a
    // directory, or its data is corrupt.
    std::optional<Bytes> read(std::string_view name) const;
    std::optional<std::string> readText(std::string_view name) const;

    // Every entry under `directory`, creating it and any intermediate
    // directories. An entry whose name would land outside `directory` - an
    // absolute path, or one climbing through ".." - is skipped rather than
    // written, so an archive from an untrusted source cannot reach past it.
    // False when any entry could not be written.
    bool extractAll(const FilePath& directory) const;

private:
    struct Impl;
    Pimpl<Impl> impl;
};

// Builds an archive in memory. Add entries, then take the bytes with finish()
// or put them on disk with writeTo(); either ends the writer.
//
// Non-copyable; move it.
class Writer
{
public:
    Writer();

    // Why the last operation failed, empty when it did not.
    std::string errorMessage() const;

    bool add(std::string_view name,
             Span<const std::uint8_t> bytes,
             Level level = Level::normal);

    bool add(std::string_view name,
             std::string_view text,
             Level level = Level::normal);

    // The file at `path` under `name`, read in full at the call.
    bool addFile(std::string_view name,
                 const FilePath& path,
                 Level level = Level::normal);

    // Every regular file under `directory`, recursively, named by its path
    // relative to `directory` under `prefix` ("" puts them at the root).
    bool addDirectory(const FilePath& directory,
                      std::string_view prefix = {},
                      Level level = Level::normal);

    // The finished archive. Empty when nothing was added or the central
    // directory could not be written; check errorMessage() to tell the two
    // apart.
    Bytes finish();

    // finish(), written with Files::writeFile. False when the archive could
    // not be finalized or the file could not be written.
    bool writeTo(const FilePath& path);

private:
    struct Impl;
    Pimpl<Impl> impl;
};

// A single blob as a zlib stream (RFC 1950): what a cache entry or a custom
// wire format wants when there is no archive to speak of.
Bytes compress(Span<const std::uint8_t> bytes, Level level = Level::normal);
Bytes compress(std::string_view text, Level level = Level::normal);

// The reverse. Nullopt when the stream is not a valid zlib stream.
std::optional<Bytes> decompress(Span<const std::uint8_t> stream);
std::optional<std::string> decompressText(Span<const std::uint8_t> stream);
} // namespace eacp::Zip

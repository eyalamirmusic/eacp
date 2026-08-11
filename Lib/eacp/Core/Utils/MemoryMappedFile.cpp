#include "MemoryMappedFile.h"
#include "MemoryMappedFilePlatform.h"

#include <algorithm>
#include <limits>

namespace eacp
{
namespace
{
// `region` is what the platform mapped; `start` is the window the caller asked
// for, which begins part-way in when the offset is not granularity-aligned.
struct Mapping
{
    Detail::MappedRegion region;
    const std::uint8_t* start = nullptr;
    std::size_t length = 0;
    bool valid = false;
};

Mapping mapOpenFile(const Detail::MappingFile& file,
                    std::uint64_t offset,
                    std::uint64_t length)
{
    if (offset > file.size)
        return {};

    // toEndOfFile is the largest uint64_t, so it clamps like any over-long ask.
    auto wanted = std::min(length, file.size - offset);

    auto mapping = Mapping {};
    mapping.valid = true;

    if (wanted == 0)
        return mapping;

    auto alignedOffset = offset - (offset % Detail::mappingGranularity());
    auto delta = static_cast<std::size_t>(offset - alignedOffset);

    // Checked before the cast, which would wrap on a 32-bit build rather than
    // fail.
    if (wanted > std::numeric_limits<std::size_t>::max() - delta)
        return {};

    mapping.region = Detail::mapRegion(
        file, alignedOffset, delta + static_cast<std::size_t>(wanted));

    if (mapping.region.address == nullptr)
        return {};

    mapping.start = static_cast<const std::uint8_t*>(mapping.region.address) + delta;
    mapping.length = static_cast<std::size_t>(wanted);

    return mapping;
}

Mapping mapWindow(const FilePath& path, std::uint64_t offset, std::uint64_t length)
{
    auto file = Detail::openForMapping(path);

    if (!file.isOpen())
        return {};

    auto mapping = mapOpenFile(file, offset, length);

    // The mapping keeps its own reference, so the descriptor has done its job.
    Detail::closeMappingFile(file);

    return mapping;
}
} // namespace

struct MemoryMappedFile::Native
{
    Native(const FilePath& path, std::uint64_t offset, std::uint64_t length)
        : mapping(mapWindow(path, offset, length))
    {
    }

    ~Native() { Detail::unmapRegion(mapping.region); }

    Native(const Native&) = delete;
    Native& operator=(const Native&) = delete;

    Mapping mapping;
};

MemoryMappedFile::MemoryMappedFile(const FilePath& path,
                                   std::uint64_t offset,
                                   std::uint64_t length)
    : filePath(path)
    , impl(path, offset, length)
{
}

bool MemoryMappedFile::isValid() const
{
    return impl->mapping.valid;
}

std::span<const std::uint8_t> MemoryMappedFile::bytes() const
{
    return {impl->mapping.start, impl->mapping.length};
}

std::string_view MemoryMappedFile::text() const
{
    auto view = bytes();

    if (view.empty())
        return {};

    return {reinterpret_cast<const char*>(view.data()), view.size()};
}

std::size_t MemoryMappedFile::size() const
{
    return impl->mapping.length;
}

bool MemoryMappedFile::empty() const
{
    return size() == 0;
}
} // namespace eacp

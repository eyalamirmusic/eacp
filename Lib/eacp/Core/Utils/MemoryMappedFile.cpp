#include "MemoryMappedFile.h"
#include "MemoryMappedFilePlatform.h"

#include <algorithm>
#include <limits>

namespace eacp
{
namespace
{
// What the platform mapped, and the window inside it the caller asked for.
// The two differ whenever the offset is not a multiple of the granularity:
// the mapping has to begin at the aligned offset below it, so the requested
// bytes start part-way into the region.
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

    // toEndOfFile is the largest std::uint64_t, so asking for everything and
    // asking for more than there is are the same clamp.
    auto wanted = std::min(length, file.size - offset);

    auto mapping = Mapping {};
    mapping.valid = true;

    if (wanted == 0)
        return mapping;

    auto alignedOffset = offset - (offset % Detail::mappingGranularity());
    auto delta = static_cast<std::size_t>(offset - alignedOffset);

    // A window can outrun the address space on a 32-bit build, where the
    // mapping would fail anyway. Checked before the cast rather than after,
    // which would wrap instead of failing.
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

    // Closed as soon as the mapping exists: mmap and MapViewOfFile each keep
    // their own reference to the file, so holding the descriptor for the life
    // of the map would pin it for nothing.
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

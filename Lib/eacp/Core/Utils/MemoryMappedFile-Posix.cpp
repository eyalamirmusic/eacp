#include "MemoryMappedFilePlatform.h"

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

namespace eacp::Detail
{
MappingFile openForMapping(const FilePath& path)
{
    // O_NONBLOCK so the open cannot hang waiting for a FIFO's writer. It costs a
    // regular file nothing, since the bytes come from the mapping.
    auto descriptor = ::open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NONBLOCK);

    if (descriptor < 0)
        return {};

    struct stat info = {};

    if (::fstat(descriptor, &info) != 0 || !S_ISREG(info.st_mode))
    {
        ::close(descriptor);
        return {};
    }

    auto file = MappingFile {};
    file.handle = descriptor;
    file.size = static_cast<std::uint64_t>(info.st_size);

    return file;
}

void closeMappingFile(MappingFile& file)
{
    if (file.isOpen())
        ::close(static_cast<int>(file.handle));

    file = {};
}

std::size_t mappingGranularity()
{
    static const auto pageSize = []
    {
        constexpr auto fallbackPageSize = std::size_t {4096};
        auto reported = ::sysconf(_SC_PAGESIZE);

        return reported > 0 ? static_cast<std::size_t>(reported) : fallbackPageSize;
    }();

    return pageSize;
}

// MAP_PRIVATE, not MAP_SHARED: nothing writes through, so the kernel is asked
// for no write-back bookkeeping.
MappedRegion mapRegion(const MappingFile& file,
                       std::uint64_t alignedOffset,
                       std::size_t length)
{
    auto* address = ::mmap(nullptr,
                           length,
                           PROT_READ,
                           MAP_PRIVATE,
                           static_cast<int>(file.handle),
                           static_cast<off_t>(alignedOffset));

    if (address == MAP_FAILED)
        return {};

    return {address, length};
}

void unmapRegion(const MappedRegion& region)
{
    if (region.address != nullptr)
        ::munmap(region.address, region.length);
}
} // namespace eacp::Detail

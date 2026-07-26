#include "MemoryMappedFilePlatform.h"

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

namespace eacp::Detail
{
MappingFile openForMapping(const FilePath& path)
{
    // O_NONBLOCK so that the open itself cannot hang: opening a FIFO for
    // reading otherwise waits for a writer to arrive, and this call has to be
    // able to reject one. It costs a regular file nothing — the flag governs
    // reads through the descriptor, and the bytes here come from the mapping.
    auto descriptor = ::open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NONBLOCK);

    if (descriptor < 0)
        return {};

    // fstat rather than stat: it answers for the file that was opened, and
    // that is the one about to be mapped.
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
        auto reported = ::sysconf(_SC_PAGESIZE);

        // sysconf answers -1 when it cannot tell. 4KB is the page size
        // everywhere this runs, and any multiple of the real one is a legal
        // mmap offset regardless.
        return reported > 0 ? static_cast<std::size_t>(reported)
                            : std::size_t {4096};
    }();

    return pageSize;
}

// MAP_PRIVATE, not MAP_SHARED: nothing here can write through the mapping, and
// a private one asks the kernel for no write-back bookkeeping at all.
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

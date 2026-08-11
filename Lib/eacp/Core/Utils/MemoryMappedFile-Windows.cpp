#include "MemoryMappedFilePlatform.h"
#include "WinInclude.h"

namespace eacp::Detail
{
MappingFile openForMapping(const FilePath& path)
{
    // Shared every way a POSIX open is. Truncation stays impossible regardless:
    // it is refused for as long as a section is open on the file.
    auto* handle =
        ::CreateFileW(path.wide().c_str(),
                      GENERIC_READ,
                      FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                      nullptr,
                      OPEN_EXISTING,
                      FILE_ATTRIBUTE_NORMAL,
                      nullptr);

    if (handle == INVALID_HANDLE_VALUE)
        return {};

    // The FILE_TYPE_DISK test is what S_ISREG does on the other side.
    auto size = LARGE_INTEGER {};

    if (::GetFileType(handle) != FILE_TYPE_DISK || !::GetFileSizeEx(handle, &size))
    {
        ::CloseHandle(handle);
        return {};
    }

    auto file = MappingFile {};
    file.handle = reinterpret_cast<std::intptr_t>(handle);
    file.size = static_cast<std::uint64_t>(size.QuadPart);

    return file;
}

void closeMappingFile(MappingFile& file)
{
    if (file.isOpen())
        ::CloseHandle(reinterpret_cast<HANDLE>(file.handle));

    file = {};
}

std::size_t mappingGranularity()
{
    static const auto granularity = []
    {
        auto info = SYSTEM_INFO {};
        ::GetSystemInfo(&info);

        return static_cast<std::size_t>(info.dwAllocationGranularity);
    }();

    return granularity;
}

MappedRegion mapRegion(const MappingFile& file,
                       std::uint64_t alignedOffset,
                       std::size_t length)
{
    // A zero maximum size means the size of the file.
    auto* section = ::CreateFileMappingW(reinterpret_cast<HANDLE>(file.handle),
                                         nullptr,
                                         PAGE_READONLY,
                                         0,
                                         0,
                                         nullptr);

    if (section == nullptr)
        return {};

    auto offset = ULARGE_INTEGER {};
    offset.QuadPart = alignedOffset;

    auto* address = ::MapViewOfFile(
        section, FILE_MAP_READ, offset.HighPart, offset.LowPart, length);

    // The view holds its own reference to the section.
    ::CloseHandle(section);

    if (address == nullptr)
        return {};

    return {address, length};
}

void unmapRegion(const MappedRegion& region)
{
    if (region.address != nullptr)
        ::UnmapViewOfFile(region.address);
}
} // namespace eacp::Detail

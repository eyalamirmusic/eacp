#pragma once

#include "FilePath.h"

#include <cstddef>
#include <cstdint>

namespace eacp::Detail
{

// Size is taken from the handle, not the path, so both describe the same file
// even if the name is replaced underneath.
struct MappingFile
{
    // A file descriptor on POSIX, a HANDLE on Windows; -1 is closed in both,
    // INVALID_HANDLE_VALUE being (HANDLE) -1.
    std::intptr_t handle = -1;
    std::uint64_t size = 0;

    bool isOpen() const { return handle != -1; }
};

// Opens for reading, and fails for anything that is not a regular file.
MappingFile openForMapping(const FilePath& path);
void closeMappingFile(MappingFile& file);

// The page size on POSIX; dwAllocationGranularity (64KB, deliberately not the
// page size) on Windows.
std::size_t mappingGranularity();

struct MappedRegion
{
    void* address = nullptr;
    std::size_t length = 0;
};

// alignedOffset must be a multiple of mappingGranularity() and leave length
// bytes inside the file. Returns an empty region on failure. Both platforms
// keep their own file reference, so the caller may close it straight after.
MappedRegion mapRegion(const MappingFile& file,
                       std::uint64_t alignedOffset,
                       std::size_t length);

void unmapRegion(const MappedRegion& region);

} // namespace eacp::Detail

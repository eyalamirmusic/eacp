#pragma once

#include "FilePath.h"

#include <cstddef>
#include <cstdint>

namespace eacp::Detail
{

// The platform half of MemoryMappedFile, implemented in
// MemoryMappedFile-Posix.cpp and MemoryMappedFile-Windows.cpp so
// MemoryMappedFile.cpp itself carries no platform switches.

// An open file and its size, taken from the handle rather than the path so
// that both describe the same file even if the name is replaced underneath.
struct MappingFile
{
    // A file descriptor on POSIX, a HANDLE on Windows; -1 is closed in both,
    // INVALID_HANDLE_VALUE being (HANDLE) -1.
    std::intptr_t handle = -1;
    std::uint64_t size = 0;

    bool isOpen() const { return handle != -1; }
};

// Opens for reading, and fails for anything that is not a regular file: a
// mapping needs a size and a page cache, and a directory, a FIFO or a device
// has neither.
MappingFile openForMapping(const FilePath& path);
void closeMappingFile(MappingFile& file);

// What a mapping offset has to be a multiple of: the page size on POSIX,
// dwAllocationGranularity on Windows — 64KB, and deliberately not the page
// size there.
std::size_t mappingGranularity();

struct MappedRegion
{
    void* address = nullptr;
    std::size_t length = 0;
};

// Maps length bytes from alignedOffset, which must be a multiple of
// mappingGranularity() and must leave length bytes inside the file. Returns
// an empty region if the mapping fails. Both platforms keep their own
// reference to the file, so the caller is free to close it as soon as this
// returns.
MappedRegion mapRegion(const MappingFile& file,
                       std::uint64_t alignedOffset,
                       std::size_t length);

void unmapRegion(const MappedRegion& region);

} // namespace eacp::Detail

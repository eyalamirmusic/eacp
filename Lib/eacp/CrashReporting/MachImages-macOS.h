#pragma once

#include <mach-o/loader.h>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>

namespace eacp::CrashReporting::Detail
{
// The span an image's code covers from its load address: its __TEXT
// segment's size, read from the load commands that follow `header`. 0 when
// the header is not a 64-bit Mach-O one or the commands run past
// `availableBytes`.
std::uint64_t getTextSegmentSize(const mach_header_64& header,
                                 std::size_t availableBytes);

// The image's LC_UUID as 8-4-4-4-12 upper-case hex, as dwarfdump prints it.
// Empty when it has none.
std::string getImageUuid(const mach_header_64& header);

struct DynamicLinkerImage
{
    const mach_header* header;
    const char* path;
};

// dyld itself, which is not among the images _dyld_image_count counts.
std::optional<DynamicLinkerImage> findDynamicLinkerImage();

// Return addresses on arm64e carry a pointer authentication code above the
// address bits.
std::uint64_t stripPointerAuthentication(std::uint64_t address);
} // namespace eacp::CrashReporting::Detail

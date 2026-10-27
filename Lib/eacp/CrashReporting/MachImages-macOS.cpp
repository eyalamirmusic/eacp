#include "MachImages-macOS.h"

#include <mach-o/dyld_images.h>
#include <mach/mach.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <string_view>

namespace eacp::CrashReporting::Detail
{
namespace
{
constexpr auto textSegmentName = std::string_view {SEG_TEXT};
constexpr auto userAddressMask = std::uint64_t {0x00007FFFFFFFFFFF};
constexpr auto uuidGroupEnds = std::to_array<std::size_t>({4, 6, 8, 10});

std::string formatUuid(const std::uint8_t (&bytes)[16])
{
    constexpr auto hexDigits = std::string_view {"0123456789ABCDEF"};
    constexpr auto nibbleBits = 4;
    constexpr auto lowNibbleMask = 0xF;
    auto text = std::string {};

    for (auto index = std::size_t {0}; index < std::size(bytes); ++index)
    {
        if (std::ranges::find(uuidGroupEnds, index) != uuidGroupEnds.end())
            text += '-';

        text += hexDigits[bytes[index] >> nibbleBits];
        text += hexDigits[bytes[index] & lowNibbleMask];
    }

    return text;
}
} // namespace

std::uint64_t getTextSegmentSize(const mach_header_64& header,
                                 std::size_t availableBytes)
{
    if (header.magic != MH_MAGIC_64)
        return 0;

    const auto* start = reinterpret_cast<const std::uint8_t*>(&header);
    auto offset = sizeof(mach_header_64);

    for (auto index = 0u; index < header.ncmds; ++index)
    {
        if (offset + sizeof(load_command) > availableBytes)
            return 0;

        auto command = load_command {};
        std::memcpy(&command, start + offset, sizeof(command));

        if (command.cmd == LC_SEGMENT_64
            && offset + sizeof(segment_command_64) <= availableBytes)
        {
            auto segment = segment_command_64 {};
            std::memcpy(&segment, start + offset, sizeof(segment));

            auto name = std::string_view {
                segment.segname, strnlen(segment.segname, sizeof(segment.segname))};

            if (name == textSegmentName)
                return segment.vmsize;
        }

        if (command.cmdsize == 0)
            return 0;

        offset += command.cmdsize;
    }

    return 0;
}

std::string getImageUuid(const mach_header_64& header)
{
    if (header.magic != MH_MAGIC_64)
        return {};

    const auto* start = reinterpret_cast<const std::uint8_t*>(&header);
    auto offset = sizeof(mach_header_64);

    for (auto index = 0u; index < header.ncmds; ++index)
    {
        auto command = load_command {};
        std::memcpy(&command, start + offset, sizeof(command));

        if (command.cmd == LC_UUID)
        {
            auto uuidCommand = uuid_command {};
            std::memcpy(&uuidCommand, start + offset, sizeof(uuidCommand));
            return formatUuid(uuidCommand.uuid);
        }

        if (command.cmdsize == 0)
            return {};

        offset += command.cmdsize;
    }

    return {};
}

std::optional<DynamicLinkerImage> findDynamicLinkerImage()
{
    auto dyldInfo = task_dyld_info_data_t {};
    auto count = mach_msg_type_number_t {TASK_DYLD_INFO_COUNT};
    auto* infoPointer = reinterpret_cast<task_info_t>(&dyldInfo);

    if (::task_info(::mach_task_self(), TASK_DYLD_INFO, infoPointer, &count)
        != KERN_SUCCESS)
        return std::nullopt;

    const auto* allImages =
        reinterpret_cast<const dyld_all_image_infos*>(dyldInfo.all_image_info_addr);

    if (allImages->dyldImageLoadAddress == nullptr || allImages->dyldPath == nullptr)
        return std::nullopt;

    return DynamicLinkerImage {allImages->dyldImageLoadAddress, allImages->dyldPath};
}

std::uint64_t stripPointerAuthentication(std::uint64_t address)
{
    return address & userAddressMask;
}
} // namespace eacp::CrashReporting::Detail

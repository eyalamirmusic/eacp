#include "TaskCapture-macOS.h"
#include "MachImages-macOS.h"

#include <mach-o/dyld_images.h>
#include <mach-o/loader.h>
#include <mach/mach_vm.h>

#include <algorithm>
#include <climits>
#include <cstring>

namespace eacp::CrashReporting::Detail
{
namespace
{
FaultingThreadRegisters makeFaultingThreadRegisters(const NativeThreadState& state)
{
    auto registers = FaultingThreadRegisters {};
    registers.state = state;
#if defined(__arm64__)
    registers.programCounter = arm_thread_state64_get_pc(state);
    registers.stackPointer = arm_thread_state64_get_sp(state);
    registers.framePointer = arm_thread_state64_get_fp(state);
    registers.linkRegister = arm_thread_state64_get_lr(state);
#else
    registers.programCounter = state.__rip;
    registers.stackPointer = state.__rsp;
    registers.framePointer = state.__rbp;
#endif
    registers.programCounter = stripPointerAuthentication(registers.programCounter);
    registers.linkRegister = stripPointerAuthentication(registers.linkRegister);
    return registers;
}

std::optional<NativeThreadState> readStateFromMessage(const MachException& exception)
{
    if (exception.stateFlavor != nativeThreadStateFlavor
        || exception.stateCount < nativeThreadStateCount)
        return std::nullopt;

    auto state = NativeThreadState {};
    std::memcpy(&state, exception.state.data(), sizeof(state));
    return state;
}

struct AppFrameRecord
{
    std::uint64_t callerFramePointer = 0;
    std::uint64_t returnAddress = 0;
};

constexpr auto framePointerAlignment = std::uint64_t {sizeof(std::uint64_t)};

std::optional<AppFrameRecord> readFrameRecord(mach_port_t task,
                                              std::uint64_t framePointer)
{
    if (framePointer == 0 || framePointer % framePointerAlignment != 0)
        return std::nullopt;

    auto record = AppFrameRecord {};
    auto bytes = std::as_writable_bytes(std::span {&record, 1});

    if (!readAppMemory(task, framePointer, bytes))
        return std::nullopt;

    record.returnAddress = stripPointerAuthentication(record.returnAddress);
    return record;
}

struct MappedRegion
{
    MemoryRange range;
    bool isInaccessible = false;
};

std::optional<MappedRegion> findRegionAtOrAbove(mach_port_t task,
                                                std::uint64_t address)
{
    auto regionStart = mach_vm_address_t {address};
    auto regionSize = mach_vm_size_t {0};
    auto depth = natural_t {0};
    auto info = vm_region_submap_short_info_data_64_t {};
    auto count = mach_msg_type_number_t {VM_REGION_SUBMAP_SHORT_INFO_COUNT_64};
    auto* infoPointer = reinterpret_cast<vm_region_recurse_info_t>(&info);

    if (::mach_vm_region_recurse(
            task, &regionStart, &regionSize, &depth, infoPointer, &count)
        != KERN_SUCCESS)
        return std::nullopt;

    auto range = MemoryRange {regionStart, regionStart + regionSize};
    return MappedRegion {range, info.protection == VM_PROT_NONE};
}

constexpr auto maxImagePathLength = std::size_t {PATH_MAX};
constexpr auto maxLoadCommandBytes = std::uint32_t {1024 * 1024};
constexpr auto pathReadChunk = std::uint64_t {256};

template <typename T>
std::optional<T> readAppValue(mach_port_t task, std::uint64_t address)
{
    auto value = T {};

    if (!readAppMemory(task, address, std::as_writable_bytes(std::span {&value, 1})))
        return std::nullopt;

    return value;
}

// In chunks that never cross a page, since the bytes after the string's end
// may be unmapped.
std::string readAppString(mach_port_t task, std::uint64_t address)
{
    auto text = std::string {};
    auto pageSize = std::uint64_t {vm_page_size};

    while (text.size() < maxImagePathLength)
    {
        auto toPageEnd = pageSize - address % pageSize;
        auto chunk = std::array<char, pathReadChunk> {};
        auto length = std::min<std::uint64_t>(toPageEnd, chunk.size());
        auto bytes = std::as_writable_bytes(std::span {chunk.data(), length});

        if (!readAppMemory(task, address, bytes))
            return text;

        auto terminator = std::find(chunk.begin(), chunk.begin() + length, '\0');
        text.append(chunk.begin(), terminator);

        if (terminator != chunk.begin() + length)
            return text;

        address += length;
    }

    return text;
}

std::optional<AppImage>
    readAppImage(mach_port_t task, std::uint64_t base, std::uint64_t pathAddress)
{
    auto header = readAppValue<mach_header_64>(task, base);

    if (!header || header->magic != MH_MAGIC_64
        || header->sizeofcmds > maxLoadCommandBytes)
        return std::nullopt;

    auto commandBytes = sizeof(mach_header_64) + header->sizeofcmds;
    auto commands = Vector<std::byte>(static_cast<int>(commandBytes));

    if (!readAppMemory(task, base, std::span {commands.data(), commandBytes}))
        return std::nullopt;

    const auto& loaded = *reinterpret_cast<const mach_header_64*>(commands.data());
    auto image = AppImage {};
    image.base = base;
    image.size = getTextSegmentSize(loaded, commandBytes);
    image.path = readAppString(task, pathAddress);
    image.uuid = findImageUuid(loaded, commandBytes).value_or(ImageUuid {});
    return image;
}

std::optional<dyld_all_image_infos> readAllImageInfos(mach_port_t task)
{
    auto dyldInfo = task_dyld_info_data_t {};
    auto count = mach_msg_type_number_t {TASK_DYLD_INFO_COUNT};
    auto* infoPointer = reinterpret_cast<task_info_t>(&dyldInfo);

    if (::task_info(task, TASK_DYLD_INFO, infoPointer, &count) != KERN_SUCCESS)
        return std::nullopt;

    return readAppValue<dyld_all_image_infos>(task, dyldInfo.all_image_info_addr);
}

// A leaf function may keep its return address in lr without a frame record
// of its own, so lr is a frame unless the first record already holds it.
void addLinkRegisterFrame(Vector<std::uint64_t>& frames,
                          const FaultingThreadRegisters& registers,
                          const std::optional<AppFrameRecord>& firstRecord)
{
    auto isAlreadyRecorded =
        firstRecord && firstRecord->returnAddress == registers.linkRegister;

    if (registers.linkRegister != 0 && !isAlreadyRecorded)
        frames.add(registers.linkRegister);
}
} // namespace

std::optional<FaultingThreadRegisters>
    readThreadState(const MachException& exception)
{
    auto state = NativeThreadState {};
    auto count = nativeThreadStateCount;
    auto* statePointer = reinterpret_cast<thread_state_t>(&state);

    if (::thread_get_state(
            exception.thread, nativeThreadStateFlavor, statePointer, &count)
        == KERN_SUCCESS)
        return makeFaultingThreadRegisters(state);

    if (auto fromMessage = readStateFromMessage(exception))
        return makeFaultingThreadRegisters(*fromMessage);

    return std::nullopt;
}

bool readAppMemory(mach_port_t task,
                   std::uint64_t address,
                   std::span<std::byte> destination)
{
    auto copied = mach_vm_size_t {0};
    auto result = ::mach_vm_read_overwrite(
        task,
        address,
        destination.size(),
        reinterpret_cast<mach_vm_address_t>(destination.data()),
        &copied);

    return result == KERN_SUCCESS && copied == destination.size();
}

// Frame records live on the stack, which grows down, so each caller's record
// sits above the last; anything else is a broken chain and ends the walk.
Vector<std::uint64_t> walkFramePointers(mach_port_t task,
                                        const FaultingThreadRegisters& registers,
                                        std::size_t maxFrames)
{
    auto frames = Vector<std::uint64_t> {registers.programCounter};
    auto record = readFrameRecord(task, registers.framePointer);
#if defined(__arm64__)
    addLinkRegisterFrame(frames, registers, record);
#endif
    auto framePointer = registers.framePointer;

    auto frameLimit = static_cast<int>(maxFrames);

    while (record && record->returnAddress != 0 && frames.size() < frameLimit)
    {
        frames.add(record->returnAddress);

        if (record->callerFramePointer <= framePointer)
            break;

        framePointer = record->callerFramePointer;
        record = readFrameRecord(task, framePointer);
    }

    return frames;
}

Vector<AppImage> readImageList(mach_port_t task)
{
    auto images = Vector<AppImage> {};
    auto allImages = readAllImageInfos(task);

    if (!allImages)
        return images;

    auto arrayAddress = reinterpret_cast<std::uintptr_t>(allImages->infoArray);

    for (auto index = 0u; index < allImages->infoArrayCount; ++index)
    {
        auto entryAddress = arrayAddress + index * sizeof(dyld_image_info);
        auto entry = readAppValue<dyld_image_info>(task, entryAddress);

        if (!entry)
            continue;

        auto base = reinterpret_cast<std::uintptr_t>(entry->imageLoadAddress);
        auto path = reinterpret_cast<std::uintptr_t>(entry->imageFilePath);

        if (auto image = readAppImage(task, base, path))
            images.add(std::move(*image));
    }

    auto dyldBase =
        reinterpret_cast<std::uintptr_t>(allImages->dyldImageLoadAddress);
    auto dyldPath = reinterpret_cast<std::uintptr_t>(allImages->dyldPath);

    if (auto dynamicLinker = readAppImage(task, dyldBase, dyldPath))
        images.add(std::move(*dynamicLinker));

    return images;
}

std::uint64_t readThreadId(mach_port_t thread)
{
    auto info = thread_identifier_info_data_t {};
    auto count = mach_msg_type_number_t {THREAD_IDENTIFIER_INFO_COUNT};
    auto* infoPointer = reinterpret_cast<thread_info_t>(&info);

    if (::thread_info(thread, THREAD_IDENTIFIER_INFO, infoPointer, &count)
        != KERN_SUCCESS)
        return 0;

    return info.thread_id;
}

std::optional<MemoryRange> findThreadStack(mach_port_t task,
                                           std::uint64_t stackPointer,
                                           std::size_t maxBytes)
{
    auto region = findRegionAtOrAbove(task, stackPointer);

    // An overflowed stack's pointer sits in the guard page below the stack.
    if (region && region->isInaccessible)
        region = findRegionAtOrAbove(task, region->range.end);

    if (!region || region->isInaccessible)
        return std::nullopt;

    auto start = std::max(stackPointer, region->range.start);
    auto end = std::min<std::uint64_t>(region->range.end, start + maxBytes);
    return MemoryRange {start, end};
}
} // namespace eacp::CrashReporting::Detail

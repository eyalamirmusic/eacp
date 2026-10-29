#include "LoadedLibrarySymbolReader-macOS.h"
#include "MachImages-macOS.h"

#include <mach-o/fat.h>
#include <mach-o/nlist.h>
#include <mach/machine.h>

#include <cstring>
#include <string_view>

namespace eacp::CrashReporting::Detail
{
namespace
{
constexpr auto imageFileTextSegmentName = std::string_view {SEG_TEXT};

#if defined(__aarch64__)
constexpr auto processCpuType = cpu_type_t {CPU_TYPE_ARM64};
#else
constexpr auto processCpuType = cpu_type_t {CPU_TYPE_X86_64};
#endif

template <typename Value>
std::optional<Value> readValue(Span<const std::uint8_t> bytes, std::size_t offset)
{
    if (offset + sizeof(Value) > bytes.getSize())
        return std::nullopt;

    auto value = Value {};
    std::memcpy(&value, bytes.data() + offset, sizeof(Value));
    return value;
}

// A universal file's header and arch table are big-endian.
std::optional<std::size_t> findSliceForThisProcess(Span<const std::uint8_t> bytes)
{
    auto fat = readValue<fat_header>(bytes, 0);

    if (!fat)
        return std::nullopt;

    if (fat->magic == MH_MAGIC_64)
        return 0;

    if (fat->magic != FAT_CIGAM)
        return std::nullopt;

    auto archCount = OSSwapBigToHostInt32(fat->nfat_arch);

    for (auto index = 0u; index < archCount; ++index)
    {
        auto archOffset = sizeof(fat_header) + index * sizeof(fat_arch);
        auto arch = readValue<fat_arch>(bytes, archOffset);

        if (!arch)
            return std::nullopt;

        auto cpuType = static_cast<cpu_type_t>(OSSwapBigToHostInt32(arch->cputype));

        if (cpuType == processCpuType)
            return OSSwapBigToHostInt32(arch->offset);
    }

    return std::nullopt;
}

struct SymbolTableLayout
{
    std::uint64_t textAddress = 0;
    symtab_command symbolTable {};
};

std::optional<SymbolTableLayout>
    readSymbolTableLayout(Span<const std::uint8_t> bytes,
                          std::size_t thisArchitectureHeaderOffset)
{
    auto header = readValue<mach_header_64>(bytes, thisArchitectureHeaderOffset);

    if (!header)
        return std::nullopt;

    auto layout = SymbolTableLayout {};
    auto hasTable = false;
    auto offset = thisArchitectureHeaderOffset + sizeof(mach_header_64);

    for (auto index = 0u; index < header->ncmds; ++index)
    {
        auto command = readValue<load_command>(bytes, offset);

        if (!command || command->cmdsize == 0)
            return std::nullopt;

        if (command->cmd == LC_SEGMENT_64)
        {
            auto segment = readValue<segment_command_64>(bytes, offset);

            if (segment
                && std::string_view {segment->segname} == imageFileTextSegmentName)
                layout.textAddress = segment->vmaddr;
        }

        if (command->cmd == LC_SYMTAB)
        {
            if (auto table = readValue<symtab_command>(bytes, offset))
            {
                layout.symbolTable = *table;
                hasTable = true;
            }
        }

        offset += command->cmdsize;
    }

    if (!hasTable)
        return std::nullopt;

    return layout;
}

bool isDefinedFunctionSymbol(const nlist_64& symbol)
{
    return (symbol.n_type & N_STAB) == 0 && (symbol.n_type & N_TYPE) == N_SECT;
}

// Mach-O prefixes every C and C++ name with an underscore.
std::string removeLeadingUnderscore(std::string_view name)
{
    if (name.starts_with('_'))
        name.remove_prefix(1);

    return std::string {name};
}
} // namespace

LoadedLibrarySymbolReader::LoadedLibrarySymbolReader(const FilePath& path)
    : file(path)
{
    if (file.isValid())
        thisArchitectureHeaderOffset = findSliceForThisProcess(file.bytes());
}

bool LoadedLibrarySymbolReader::isValid() const
{
    return thisArchitectureHeaderOffset.has_value();
}

std::string LoadedLibrarySymbolReader::getBuildUuid() const
{
    if (!thisArchitectureHeaderOffset)
        return {};

    auto bytes = file.bytes();
    auto header = readValue<mach_header_64>(bytes, *thisArchitectureHeaderOffset);

    if (!header
        || *thisArchitectureHeaderOffset + sizeof(mach_header_64)
                   + header->sizeofcmds
               > bytes.getSize())
    {
        return {};
    }

    const auto& mapped = *reinterpret_cast<const mach_header_64*>(
        bytes.data() + *thisArchitectureHeaderOffset);
    return getImageUuid(mapped);
}

std::optional<FunctionContainingOffset>
    LoadedLibrarySymbolReader::findFunctionContainingOffset(
        std::uint64_t offset) const
{
    if (!thisArchitectureHeaderOffset)
        return std::nullopt;

    auto bytes = file.bytes();
    auto layout = readSymbolTableLayout(bytes, *thisArchitectureHeaderOffset);

    if (!layout)
        return std::nullopt;

    auto target = layout->textAddress + offset;
    auto tableStart = *thisArchitectureHeaderOffset + layout->symbolTable.symoff;
    auto namesStart = *thisArchitectureHeaderOffset + layout->symbolTable.stroff;
    auto best = std::optional<nlist_64> {};

    for (auto index = 0u; index < layout->symbolTable.nsyms; ++index)
    {
        auto symbol =
            readValue<nlist_64>(bytes, tableStart + index * sizeof(nlist_64));

        if (!symbol)
            return std::nullopt;

        auto isCandidate =
            isDefinedFunctionSymbol(*symbol) && symbol->n_value <= target;

        if (isCandidate && (!best || symbol->n_value > best->n_value))
            best = symbol;
    }

    if (!best)
        return std::nullopt;

    auto nameOffset = namesStart + best->n_un.n_strx;

    if (nameOffset >= bytes.getSize())
        return std::nullopt;

    const auto* nameStart = reinterpret_cast<const char*>(bytes.data() + nameOffset);
    auto name = std::string_view {
        nameStart, ::strnlen(nameStart, bytes.getSize() - nameOffset)};
    return FunctionContainingOffset {removeLeadingUnderscore(name),
                                     target - best->n_value};
}
} // namespace eacp::CrashReporting::Detail

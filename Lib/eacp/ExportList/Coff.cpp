#include "Coff.h"
#include "ByteReader.h"

#include <eacp/Core/Utils/Format.h>
#include <eacp/Core/Utils/Strings.h>

#include <algorithm>
#include <array>
#include <cstring>

namespace eacp::ExportList
{
namespace
{
constexpr auto classicHeaderSize = std::size_t {20};
constexpr auto bigObjHeaderSize = std::size_t {56};
constexpr auto sectionHeaderSize = std::size_t {40};
constexpr auto classicSymbolSize = std::size_t {18};
constexpr auto bigObjSymbolSize = std::size_t {20};

constexpr auto storageClassExternal = std::uint8_t {2};
constexpr auto storageClassStatic = std::uint8_t {3};
constexpr auto selectionOffsetInAux = std::uint64_t {14};
constexpr auto selectNoDuplicates = std::uint8_t {1};
constexpr auto selectLargest = std::uint8_t {6};
constexpr auto complexTypeFunction = std::uint16_t {2};

constexpr auto sectionComdat = std::uint32_t {0x00001000};
constexpr auto sectionExecute = std::uint32_t {0x20000000};
constexpr auto sectionRead = std::uint32_t {0x40000000};
constexpr auto sectionWrite = std::uint32_t {0x80000000};

constexpr auto anonymousSignature = std::uint16_t {0xFFFF};
constexpr auto lastClassicSection = std::uint16_t {0xFEFF};

// {D1BAA1C7-BAEE-4BA9-AF20-FAF66AA4DCB8}, as the bytes of a GUID on disk.
constexpr auto bigObjClassId = std::string_view {
    "\xC7\xA1\xBA\xD1\xEE\xBA\xA9\x4B\xAF\x20\xFA\xF6\x6A\xA4\xDC\xB8", 16};

constexpr auto knownCoffMachines = std::array<std::uint16_t, 7> {
    0x0000, 0x8664, 0xAA64, 0x014C, 0xA641, 0xA64E, 0x01C4};

std::string_view refusedMachineName(std::uint16_t machine)
{
    switch (machine)
    {
        case 0x014C:
            return " (x86)";
        case 0xA641:
            return " (ARM64EC)";
        case 0xA64E:
            return " (ARM64X)";
        case 0x01C4:
            return " (ARM)";
        default:
            return "";
    }
}

// A compiled resource file starts with an empty resource entry: sizes 0 and
// 0x20, then 0xFFFF type and name markers.
bool isCompiledResource(const ByteReader& reader)
{
    return reader.size() >= 32 && reader.u32(0) == 0 && reader.u32(4) == 0x20
           && reader.u16(8) == 0xFFFF && reader.u16(12) == 0xFFFF;
}

struct Layout
{
    Machine machine = Machine::Unknown;
    std::uint64_t sectionCount = 0;
    std::uint64_t sectionTable = 0;
    std::uint64_t symbolCount = 0;
    std::uint64_t symbolTable = 0;
    std::size_t symbolSize = classicSymbolSize;
    bool bigObj = false;
};

bool isBitcode(const ByteReader& reader)
{
    if (reader.size() < 4)
        return false;

    auto magic = reader.u32(0);
    return magic == 0xDEC04342u || magic == 0x0B17C0DEu;
}

bool isAnonymous(const ByteReader& reader)
{
    return reader.size() >= 6 && reader.u16(0) == 0
           && reader.u16(2) == anonymousSignature;
}

bool hasBigObjClassId(const ByteReader& reader)
{
    if (reader.size() < bigObjHeaderSize)
        return false;

    return std::memcmp(
               reader.data() + 12, bigObjClassId.data(), bigObjClassId.size())
           == 0;
}

ObjectFormat anonymousFormat(const ByteReader& reader)
{
    auto version = reader.u16(4);

    if (version == 0)
        return ObjectFormat::Import;

    if (version >= 2 && hasBigObjClassId(reader))
        return ObjectFormat::BigObj;

    return ObjectFormat::LinkTimeCode;
}

bool isKnownCoffMachine(std::uint16_t machine)
{
    return std::find(knownCoffMachines.begin(), knownCoffMachines.end(), machine)
           != knownCoffMachines.end();
}

std::string describe(std::string_view displayName, std::string_view problem)
{
    return Strings::concat(displayName, ": ", problem);
}

[[noreturn]] void refuse(std::string_view displayName, std::string_view problem)
{
    throw Error(describe(displayName, problem));
}

[[noreturn]] void refuseLinkTimeCode(std::string_view displayName,
                                     std::string_view what)
{
    refuse(displayName,
           Strings::concat(
               "is ",
               what,
               ", not a COFF object, so the symbols it defines cannot be listed. "
               "Link-time optimisation (/GL, -flto) must be off for every "
               "library and object in the hot-reload closure."));
}

Layout classicLayout(const ByteReader& reader, std::string_view displayName)
{
    if (reader.size() < classicHeaderSize)
        refuse(displayName, "is too short for a COFF header");

    auto layout = Layout {};
    layout.machine = Machine {reader.u16(0)};
    layout.sectionCount = reader.u16(2);
    layout.symbolTable = reader.u32(8);
    layout.symbolCount = reader.u32(12);
    layout.sectionTable = classicHeaderSize + reader.u16(16);
    return layout;
}

Layout bigObjLayout(const ByteReader& reader, std::string_view displayName)
{
    if (reader.size() < bigObjHeaderSize)
        refuse(displayName, "is too short for a /bigobj header");

    auto layout = Layout {};
    layout.machine = Machine {reader.u16(6)};
    layout.sectionCount = reader.u32(44);
    layout.symbolTable = reader.u32(48);
    layout.symbolCount = reader.u32(52);
    layout.sectionTable = bigObjHeaderSize;
    layout.symbolSize = bigObjSymbolSize;
    layout.bigObj = true;
    return layout;
}

void checkMachine(Machine machine, std::string_view displayName)
{
    if (machine == Machine::Unknown || machine == Machine::X64
        || machine == Machine::Arm64)
        return;

    auto raw = static_cast<std::uint16_t>(machine);
    refuse(displayName,
           Strings::format("is for machine 0x{:04X}{}; only x64 and ARM64 objects "
                           "are supported",
                           static_cast<unsigned>(raw),
                           refusedMachineName(raw)));
}

class SymbolTable
{
public:
    SymbolTable(const ByteReader& readerToUse,
                const Layout& layoutToUse,
                std::string_view displayNameToUse)
        : reader(readerToUse)
        , layout(layoutToUse)
        , displayName(displayNameToUse)
    {
        checkBounds();
        readComdatSelections();
    }

    void visitDefinedExternals(const SymbolVisitor& visit) const
    {
        for (auto index = std::uint64_t {0}; index < layout.symbolCount; ++index)
        {
            auto offset = symbolOffset(index);
            auto auxCount = reader.u8(offset + layout.symbolSize - 1);

            if (auto symbol = definedExternal(offset))
                visit(*symbol);

            index += auxCount;
        }
    }

private:
    // A COMDAT section's selection sits in the auxiliary record of the static
    // symbol that defines the section.
    void readComdatSelections()
    {
        selections.resize(static_cast<int>(layout.sectionCount), std::uint8_t {0});

        for (auto index = std::uint64_t {0}; index < layout.symbolCount; ++index)
        {
            auto offset = symbolOffset(index);
            auto auxCount = reader.u8(offset + layout.symbolSize - 1);

            if (auxCount > 0 && index + 1 < layout.symbolCount)
                recordSelection(offset);

            index += auxCount;
        }
    }

    void recordSelection(std::uint64_t offset)
    {
        if (storageClass(offset) != storageClassStatic
            || reader.u32(offset + 8) != 0)
            return;

        auto number = sectionNumber(offset);

        if (number <= 0 || number > static_cast<std::int64_t>(layout.sectionCount))
            return;

        auto& selection = selections[static_cast<int>(number - 1)];

        if (selection == 0 && (sectionCharacteristics(number) & sectionComdat))
            selection = reader.u8(offset + layout.symbolSize + selectionOffsetInAux);
    }

    // A COMDAT the linker picks one copy of, from however many objects define
    // it. /Gy's no-duplicates COMDATs are ordinary definitions in a section of
    // their own.
    bool isDuplicableComdat(std::int64_t number) const
    {
        auto selection = selections[static_cast<int>(number - 1)];
        return selection > selectNoDuplicates && selection <= selectLargest;
    }

    void checkBounds() const
    {
        auto sectionsEnd =
            layout.sectionTable + layout.sectionCount * sectionHeaderSize;

        if (sectionsEnd > reader.size())
            refuse(displayName, "has a section table past the end of the file");

        auto symbolsEnd = stringTableOffset();

        if (layout.symbolCount > 0 && symbolsEnd > reader.size())
            refuse(displayName, "has a symbol table past the end of the file");
    }

    std::uint64_t stringTableOffset() const
    {
        return layout.symbolTable + layout.symbolCount * layout.symbolSize;
    }

    std::uint64_t symbolOffset(std::uint64_t index) const
    {
        return layout.symbolTable + index * layout.symbolSize;
    }

    std::int64_t sectionNumber(std::uint64_t offset) const
    {
        if (layout.bigObj)
            return static_cast<std::int32_t>(reader.u32(offset + 12));

        return classicSectionNumber(reader.u16(offset + 12));
    }

    // 1 to 0xFEFF are sections, 0x8000 and up included; only the values above
    // are special (0xFFFF absolute, 0xFFFE debug), and read as negative.
    static std::int64_t classicSectionNumber(std::uint16_t raw)
    {
        if (raw > lastClassicSection)
            return static_cast<std::int16_t>(raw);

        return raw;
    }

    std::uint16_t type(std::uint64_t offset) const
    {
        return reader.u16(offset + (layout.bigObj ? 16 : 14));
    }

    std::uint8_t storageClass(std::uint64_t offset) const
    {
        return reader.u8(offset + (layout.bigObj ? 18 : 16));
    }

    std::uint32_t sectionCharacteristics(std::int64_t number) const
    {
        if (number > static_cast<std::int64_t>(layout.sectionCount))
            refuse(displayName, "has a symbol in a section that does not exist");

        auto header = layout.sectionTable
                      + static_cast<std::uint64_t>(number - 1) * sectionHeaderSize;
        return reader.u32(header + 36);
    }

    std::string_view name(std::uint64_t offset) const
    {
        if (reader.u32(offset) != 0)
            return reader.paddedString(offset, 8);

        auto table = stringTableOffset();
        auto position = table + reader.u32(offset + 4);

        if (position >= reader.size())
            refuse(displayName, "has a symbol name past the end of the file");

        auto text = reader.cString(position);

        if (!text)
            refuse(displayName, "has an unterminated symbol name");

        return *text;
    }

    static SymbolKind kindOf(std::uint16_t symbolType, std::uint32_t characteristics)
    {
        auto isFunction = ((symbolType >> 4) & 0x3) == complexTypeFunction;

        if (isFunction)
            return SymbolKind::Code;

        if (characteristics & sectionWrite)
            return SymbolKind::Data;

        if ((characteristics & sectionExecute) || !(characteristics & sectionRead))
            return SymbolKind::Code;

        return SymbolKind::ReadOnlyData;
    }

    std::optional<DefinedSymbol> definedExternal(std::uint64_t offset) const
    {
        if (storageClass(offset) != storageClassExternal)
            return std::nullopt;

        auto number = sectionNumber(offset);

        if (number <= 0)
            return std::nullopt;

        auto characteristics = sectionCharacteristics(number);

        return DefinedSymbol {name(offset),
                              kindOf(type(offset), characteristics),
                              isDuplicableComdat(number)};
    }

    const ByteReader& reader;
    const Layout& layout;
    std::string_view displayName;
    Vector<std::uint8_t> selections;
};
} // namespace

std::string_view machineName(Machine machine)
{
    switch (machine)
    {
        case Machine::X64:
            return "X64";
        case Machine::Arm64:
            return "ARM64";
        case Machine::Unknown:
            break;
    }

    return "unknown";
}

ObjectFormat detectObjectFormat(Bytes bytes)
{
    auto reader = ByteReader {bytes};

    if (isBitcode(reader))
        return ObjectFormat::Bitcode;

    if (isAnonymous(reader))
        return anonymousFormat(reader);

    if (reader.size() >= classicHeaderSize && isKnownCoffMachine(reader.u16(0)))
        return ObjectFormat::Coff;

    return ObjectFormat::Unknown;
}

Machine readObjectSymbols(Bytes bytes,
                          std::string_view displayName,
                          const SymbolVisitor& visit)
{
    auto reader = ByteReader {bytes, displayName};
    auto layout = Layout {};

    if (isCompiledResource(reader))
        refuse(displayName, "is a compiled resource file (.res), not an object");

    switch (detectObjectFormat(bytes))
    {
        case ObjectFormat::Coff:
            layout = classicLayout(reader, displayName);
            break;
        case ObjectFormat::BigObj:
            layout = bigObjLayout(reader, displayName);
            break;
        case ObjectFormat::Import:
        {
            auto machine =
                Machine {reader.size() >= 8 ? reader.u16(6) : std::uint16_t {0}};
            checkMachine(machine,
                         Strings::concat(displayName, " (an import member)"));
            return machine;
        }
        case ObjectFormat::LinkTimeCode:
            refuseLinkTimeCode(displayName,
                               "MSVC link-time code generation output (/GL)");
        case ObjectFormat::Bitcode:
            refuseLinkTimeCode(displayName, "LLVM bitcode");
        case ObjectFormat::Unknown:
            refuse(displayName, "is neither a COFF object nor an archive");
    }

    checkMachine(layout.machine, displayName);
    SymbolTable(reader, layout, displayName).visitDefinedExternals(visit);
    return layout.machine;
}
} // namespace eacp::ExportList

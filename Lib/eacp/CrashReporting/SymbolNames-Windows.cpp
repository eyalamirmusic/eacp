#include "SymbolNames-Windows.h"

#include <eacp/Core/Utils/Strings.h>

#include <array>
#include <cstdio>

namespace eacp::CrashReporting::Detail
{
namespace
{
constexpr auto maxSymbolNameLength = std::size_t {1024};

std::string findSymbolName(HANDLE process, std::uint64_t address)
{
    auto buffer =
        std::array<std::byte,
                   sizeof(SYMBOL_INFOW) + maxSymbolNameLength * sizeof(wchar_t)> {};
    auto& symbol = *reinterpret_cast<SYMBOL_INFOW*>(buffer.data());
    symbol.SizeOfStruct = sizeof(SYMBOL_INFOW);
    symbol.MaxNameLen = static_cast<ULONG>(maxSymbolNameLength);
    auto displacement = DWORD64 {0};

    if (!::SymFromAddrW(process, address, &displacement, &symbol))
        return {};

    auto name = Strings::narrow(std::wstring_view {symbol.Name, symbol.NameLen});
    return name + " + " + std::to_string(displacement);
}

bool hasPdbSignature(const IMAGEHLP_MODULEW64& module)
{
    return module.PdbSig70 != GUID {};
}

// The PDB's CodeView GUID and age: lower-case 8-4-4-4-12
// followed by -<age hex>.
std::string formatDebugId(const GUID& guid, DWORD age)
{
    constexpr auto debugIdLength = std::size_t {64};
    auto text = std::array<char, debugIdLength> {};
    std::snprintf(text.data(),
                  text.size(),
                  "%08lx-%04hx-%04hx-%02x%02x-%02x%02x%02x%02x%02x%02x-%lx",
                  guid.Data1,
                  guid.Data2,
                  guid.Data3,
                  guid.Data4[0],
                  guid.Data4[1],
                  guid.Data4[2],
                  guid.Data4[3],
                  guid.Data4[4],
                  guid.Data4[5],
                  guid.Data4[6],
                  guid.Data4[7],
                  age);

    return text.data();
}

} // namespace

StackFrame
    makeReportFrame(HANDLE process, std::uint64_t address, bool isReturnAddress)
{
    auto frame = StackFrame {};

    // Symbols first: with deferred loads the PDB, and so its signature, is
    // only read once something asks for a symbol in the module.
    auto lookupBackoff = std::uint64_t {isReturnAddress ? 1u : 0u};
    frame.symbol = findSymbolName(process, address - lookupBackoff);

    auto module = IMAGEHLP_MODULEW64 {};
    module.SizeOfStruct = sizeof(module);

    if (::SymGetModuleInfoW64(process, address, &module))
    {
        frame.modulePath = Strings::narrow(module.LoadedImageName);
        frame.moduleBase = module.BaseOfImage;

        if (hasPdbSignature(module))
            frame.moduleUuid = formatDebugId(module.PdbSig70, module.PdbAge);
    }

    frame.offset = address - frame.moduleBase;
    return frame;
}

std::string_view getExceptionTypeName(const std::type_info& type)
{
    auto name = std::string_view {type.name()};
    constexpr auto classPrefix = std::string_view {"class "};

    if (name.starts_with(classPrefix))
        name.remove_prefix(classPrefix.size());

    return name;
}
} // namespace eacp::CrashReporting::Detail

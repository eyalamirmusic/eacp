#pragma once

#include <eacp/Core/Utils/Containers.h>

#include <cstdint>
#include <functional>
#include <stdexcept>
#include <string>
#include <string_view>

// Reading the symbol tables of COFF object files: the classic layout and the
// /bigobj one, from MSVC cl and from clang-cl, for x64 and ARM64.
namespace eacp::ExportList
{
// Every refusal: a corrupt or unknown input, a link-time-optimised object, or
// a list that cannot be written. The message names the file.
struct Error : std::runtime_error
{
    using std::runtime_error::runtime_error;
};

using Bytes = Span<const std::uint8_t>;

enum class Machine : std::uint16_t
{
    Unknown = 0,
    X64 = 0x8664,
    Arm64 = 0xAA64,
};

std::string_view machineName(Machine machine);

enum class ObjectFormat
{
    Coff,
    BigObj,
    // A short import object, as an import library holds: it defines nothing.
    Import,
    // MSVC's /GL intermediate code, an anonymous object of another class.
    LinkTimeCode,
    // LLVM bitcode, raw or in its wrapper, from clang-cl's /GL or -flto.
    Bitcode,
    Unknown,
};

ObjectFormat detectObjectFormat(Bytes bytes);

// Code: a function, or anything in an executable section. Data: writable data.
// ReadOnlyData: constants, string literals, vftables, RTTI.
enum class SymbolKind
{
    Code,
    Data,
    ReadOnlyData,
};

// An external symbol the object defines in one of its own sections. The name
// views the object's bytes. comdat: a COMDAT the linker picks one copy of from
// any number of objects (inline functions, templates, inline variables);
// /Gy's and /Gw's no-duplicates COMDATs are ordinary definitions.
struct DefinedSymbol
{
    std::string_view name;
    SymbolKind kind = SymbolKind::Code;
    bool comdat = false;
};

using SymbolVisitor = std::function<void(const DefinedSymbol&)>;

// Calls visit for every external symbol the object defines, and answers its
// machine. An import object visits nothing. Throws Error, naming displayName,
// for link-time code, bitcode, an unknown format or a corrupt table.
Machine readObjectSymbols(Bytes bytes,
                          std::string_view displayName,
                          const SymbolVisitor& visit);
} // namespace eacp::ExportList

#pragma once

#include "CrashReport.h"

#include <eacp/Core/Utils/WinInclude.h>

#include <dbghelp.h>

#include <string_view>
#include <typeinfo>

// Naming what an address or a type is, through DbgHelp, for the handler's
// stack walk and the app's own events alike.
namespace eacp::CrashReporting::Detail
{
// The frame at `address` in `process`, which SymInitializeW has been called
// for: symbol, module and the PDB's debug id. A return address is looked up
// one byte back, at its call.
StackFrame
    makeReportFrame(HANDLE process, std::uint64_t address, bool isReturnAddress);

// The type's name as source spells it, without MSVC's "class " prefix.
std::string_view getExceptionTypeName(const std::type_info& type);
} // namespace eacp::CrashReporting::Detail

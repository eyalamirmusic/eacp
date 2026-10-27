#pragma once

#include <eacp/Core/Utils/WinInclude.h>

#include <optional>
#include <string_view>

namespace eacp::CrashReporting::Detail
{
// The NTSTATUS name of an exception code, which is also what a process that
// ended through one exits with.
std::optional<std::string_view> findExceptionName(DWORD code);
} // namespace eacp::CrashReporting::Detail

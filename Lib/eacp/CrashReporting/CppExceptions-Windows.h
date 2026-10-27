#pragma once

#include <eacp/Core/Utils/WinInclude.h>

#include <functional>
#include <string_view>

namespace eacp::CrashReporting::Detail
{
// A C++ exception nothing caught reaches the unhandled-exception filter as a
// structured exception, and std::current_exception no longer sees it there.
bool isCppException(const EXCEPTION_RECORD& record);

// The thrown type's name and, when it derives from std::exception, its what(),
// read from the record the way the MSVC runtime lays it out. Calls `record`
// with the two, which point into memory that lives only for the call.
void readCppException(
    const EXCEPTION_RECORD& record,
    const std::function<void(std::string_view, std::string_view)>& recordMessage);
} // namespace eacp::CrashReporting::Detail

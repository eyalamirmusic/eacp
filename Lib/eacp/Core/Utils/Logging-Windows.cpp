#include "LoggingPlatform.h"
#include "Common.h"

#include "Strings.h"
#include "WinInclude.h"

namespace eacp::Detail
{

std::tm localTime(std::time_t time)
{
    auto result = std::tm {};
    localtime_s(&result, &time);
    return result;
}

void platformDebugOutput(std::string_view line)
{
    auto withNewline = std::string {line};
    withNewline += '\n';

    // OutputDebugStringW wants a terminated string; c_str() supplies the NUL.
    OutputDebugStringW(Strings::widen(withNewline).c_str());
}

} // namespace eacp::Detail

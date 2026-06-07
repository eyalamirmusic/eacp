#include "LoggingPlatform.h"

#include "WinInclude.h"

#include <string>

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

    auto required = MultiByteToWideChar(CP_UTF8,
                                        0,
                                        withNewline.data(),
                                        static_cast<int>(withNewline.size()),
                                        nullptr,
                                        0);
    if (required <= 0)
        return;

    auto wide = std::wstring(static_cast<std::size_t>(required), L'\0');
    MultiByteToWideChar(CP_UTF8,
                        0,
                        withNewline.data(),
                        static_cast<int>(withNewline.size()),
                        wide.data(),
                        required);
    wide.push_back(L'\0');
    OutputDebugStringW(wide.c_str());
}

} // namespace eacp::Detail

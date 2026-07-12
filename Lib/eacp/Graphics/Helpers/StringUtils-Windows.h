#pragma once

#include <eacp/Core/Utils/WinInclude.h>

#include <string>

namespace eacp::Graphics
{

inline std::wstring toWideString(const std::string& utf8)
{
    if (utf8.empty())
        return {};

    auto length = MultiByteToWideChar(
        CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
    if (length <= 0)
        return {};

    auto wide = std::wstring(static_cast<size_t>(length), L'\0');
    MultiByteToWideChar(
        CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), wide.data(), length);
    return wide;
}

inline std::string fromWideString(const std::wstring& wide)
{
    if (wide.empty())
        return {};

    auto length = WideCharToMultiByte(CP_UTF8,
                                      0,
                                      wide.data(),
                                      static_cast<int>(wide.size()),
                                      nullptr,
                                      0,
                                      nullptr,
                                      nullptr);
    if (length <= 0)
        return {};

    auto utf8 = std::string(static_cast<size_t>(length), '\0');
    WideCharToMultiByte(CP_UTF8,
                        0,
                        wide.data(),
                        static_cast<int>(wide.size()),
                        utf8.data(),
                        length,
                        nullptr,
                        nullptr);
    return utf8;
}

} // namespace eacp::Graphics

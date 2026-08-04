#pragma once

#include <eacp/Core/Utils/WinInclude.h>

#include <string>

namespace eacp::Graphics
{

// The UTF-8 -> UTF-16 direction lives in eacp::Strings::widen (Core/Utils/Strings.h),
// which is portable; this is the Windows-only return leg.
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

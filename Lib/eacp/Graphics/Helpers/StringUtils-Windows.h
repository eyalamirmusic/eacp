#pragma once

#include <string>
#include <winrt/base.h>

namespace eacp::Graphics
{

inline std::wstring toWideString(const std::string& utf8)
{
    if (utf8.empty())
        return {};
    winrt::hstring h = winrt::to_hstring(utf8);
    return std::wstring(h.c_str(), h.size());
}

inline std::string fromWideString(const std::wstring& wide)
{
    if (wide.empty())
        return {};
    return winrt::to_string(winrt::hstring{wide});
}

} // namespace eacp::Graphics

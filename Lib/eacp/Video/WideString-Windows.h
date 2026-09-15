#pragma once

#include <eacp/Core/Utils/WinInclude.h>

#include <string>

namespace eacp::Video
{

// FilePath carries UTF-8; Media Foundation wants a wide URL. Shared by the
// encoder and decoder backends: a copy in each file's anonymous namespace
// collides when the unity build folds both into one translation unit.
inline std::wstring widen(const char* utf8)
{
    auto length = MultiByteToWideChar(CP_UTF8, 0, utf8, -1, nullptr, 0);
    if (length <= 0)
        return {};

    std::wstring wide(static_cast<std::size_t>(length), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8, -1, wide.data(), length);
    if (!wide.empty() && wide.back() == L'\0')
        wide.pop_back();

    return wide;
}

} // namespace eacp::Video

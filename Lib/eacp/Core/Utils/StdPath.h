#pragma once

#include "FilePath.h"

#include <filesystem>

// The FilePath -> std::filesystem boundary, for implementation files and
// callers that need real path algebra. Public headers only see FilePath.
namespace eacp
{
inline std::filesystem::path toStdPath(const FilePath& path)
{
    // Wide on Windows via wide(), which never throws — the u8string route
    // throws on text that is not valid UTF-8. Elsewhere the native encoding
    // is the text as-is.
    if constexpr (sizeof(std::filesystem::path::value_type) == sizeof(wchar_t))
        return std::filesystem::path {path.wide()};
    else
        return std::filesystem::path {path.str()};
}
} // namespace eacp

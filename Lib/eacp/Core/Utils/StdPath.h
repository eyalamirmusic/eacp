#pragma once

#include "FilePath.h"

#include <filesystem>

// The FilePath -> std::filesystem boundary, for implementation files only.
namespace eacp
{
inline std::filesystem::path toStdPath(const FilePath& path)
{
    // Via wide(), which never throws, unlike the u8string route.
    if constexpr (sizeof(std::filesystem::path::value_type) == sizeof(wchar_t))
        return std::filesystem::path {path.wide()};
    else
        return std::filesystem::path {path.str()};
}
} // namespace eacp

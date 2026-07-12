#pragma once

#include "FilePath.h"

#include <filesystem>

// The FilePath -> std::filesystem boundary, for implementation files and
// callers that need real path algebra. Public headers only see FilePath.
namespace eacp
{
inline std::filesystem::path toStdPath(const FilePath& path)
{
    auto& text = path.str();
    return std::filesystem::path {std::u8string {text.begin(), text.end()}};
}
} // namespace eacp

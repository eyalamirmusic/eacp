#pragma once

#include "FilePath.h"

#include <filesystem>

// The FilePath -> std::filesystem boundary, for implementation files and
// callers that need real path algebra. Public headers only see FilePath.
namespace eacp
{
std::filesystem::path toStdPath(const FilePath& path);
} // namespace eacp

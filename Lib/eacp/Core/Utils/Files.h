#pragma once

#include "Common.h"
#include "FilePath.h"

#include <span>

namespace eacp::Files
{
std::string readFile(const FilePath& path);

// Writes bytes to path, creating parent directories first. Throws
// std::runtime_error when the file can't be opened or fully written.
void writeFile(const FilePath& path, std::span<const std::uint8_t> bytes);

std::string getBundleResourcePath(const std::string& filename);
std::string filenameFromPath(const std::string& path);
} // namespace eacp::Files

#pragma once

#include "Common.h"
#include "FilePath.h"

#include <span>

namespace eacp::Files
{
std::string readFile(const FilePath& path);

// Creates parent directories first. Throws std::runtime_error when the file
// can't be opened or fully written.
void writeFile(const FilePath& path, std::span<const std::uint8_t> bytes);

// Writes via a temporary sibling and a rename, so a concurrent reader sees the
// whole old file or the whole new one. Follows symlinks and preserves an
// existing file's permission bits. Throws std::runtime_error like writeFile.
void writeFileAtomically(const FilePath& path, std::span<const std::uint8_t> bytes);

std::string getBundleResourcePath(const std::string& filename);
std::string filenameFromPath(const std::string& path);
} // namespace eacp::Files

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

// Writes bytes so a concurrent reader sees either the whole old file or the
// whole new one, never a half-written mix: the data goes to a temporary
// sibling and is then renamed over the target, which the filesystem does
// atomically. Losing power or running out of disk part-way leaves the
// original untouched, which plain writeFile — opening the target with trunc —
// cannot promise.
//
// Two details a bare rename would get wrong, both of which lose information
// that was on the file before the save:
//
// - Symlinks are followed, so writing through one replaces what it points at
//   rather than turning the link into a regular file.
// - An existing file's permission bits are carried over, so saving a script
//   does not silently un-execute it by handing the replacement the umask.
//
// Throws std::runtime_error, like writeFile, if the write or the rename fails.
void writeFileAtomically(const FilePath& path, std::span<const std::uint8_t> bytes);

std::string getBundleResourcePath(const std::string& filename);
std::string filenameFromPath(const std::string& path);
} // namespace eacp::Files

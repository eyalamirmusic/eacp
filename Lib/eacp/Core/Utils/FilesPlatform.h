#pragma once

#include "FilePath.h"

#include <filesystem>
#include <string>

namespace eacp::Detail
{

// The bundle's own resource lookup, implemented in Files.mm and empty on
// platforms with no bundle, so Files.cpp carries no platform switches.
std::string bundleResourcePath(const std::string& filename);

// The platform's own notion of hidden beyond a leading '.': the hidden
// attribute on Windows, Finder's UF_HIDDEN flag on Apple platforms, nothing on
// Linux, whose filesystems carry no such flag.
bool hasHiddenAttribute(const std::filesystem::path& path);

#if defined(_WIN32)
// The full path of a loaded module (an HMODULE; null is the executable),
// however long. Implemented in Files-Windows.cpp.
FilePath modulePath(void* module);
#endif

} // namespace eacp::Detail

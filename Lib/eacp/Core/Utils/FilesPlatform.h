#pragma once

#include "FilePath.h"

#include <filesystem>
#include <string>
#include <string_view>

namespace eacp::Detail
{

// text with every 'from' replaced by 'to': the separator swap behind
// FilePath::nativeString() and fromNative(), with the platform's choice left
// to the caller so every platform's tests can exercise the Windows form. Safe
// on UTF-8 for ASCII separators: no byte of a multi-byte sequence is below
// 0x80. Implemented in FilePath.cpp.
std::string replaceSeparators(std::string_view text, char from, char to);

// The bundle's own resource lookup, implemented in Files.mm and empty on
// platforms with no bundle, so Files.cpp carries no platform switches.
std::string bundleResourcePath(const std::string& filename);

// The platform's own notion of hidden beyond a leading '.': the hidden
// attribute on Windows, Finder's UF_HIDDEN flag on Apple platforms, nothing on
// Linux, whose filesystems carry no such flag.
bool hasHiddenAttribute(const std::filesystem::path& path);

// What Files::openWithSystem and revealInFileManager hand the system: path
// made absolute and lexically normal, so the handler sees the same item
// whatever its own working directory is. Empty when path is empty or nothing
// is there. Implemented in Files.cpp.
FilePath systemTarget(const FilePath& path);

// The platform halves of Files::openWithSystem and revealInFileManager, in
// FilesOpen-<platform>. Only ever called with a non-empty systemTarget.
bool openWithSystem(const FilePath& target);
bool revealInFileManager(const FilePath& target);

#if defined(_WIN32)
// The full path of a loaded module (an HMODULE; null is the executable),
// however long. Implemented in Files-Windows.cpp.
FilePath modulePath(void* module);
#endif

} // namespace eacp::Detail

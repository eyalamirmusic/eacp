#pragma once

#include "FilePath.h"

#include <filesystem>
#include <functional>
#include <string>
#include <string_view>
#include <system_error>

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

// writeFileAtomically's last step: the file at from takes the place of the one
// at to in a single rename, so to is always either the old file or the new
// one. std::filesystem::rename everywhere but Windows, whose half is in
// Files-Windows.cpp. Empty on success.
std::error_code replaceFile(const std::filesystem::path& from,
                            const std::filesystem::path& to);

#if defined(_WIN32)
// The full path of a loaded module (an HMODULE; null is the executable),
// however long. Implemented in Files-Windows.cpp.
FilePath modulePath(void* module);

// Windows' replaceFile is a sequence of steps, each tried only when the one
// before failed in a way nextReplaceStep (pure, so the tests cover every
// answer) says the next can get past. The steps and the driver are exposed so
// the tests reach what only older systems, FAT, exFAT and network shares take.
enum class ReplaceStep
{
    renameIgnoringReadOnly,
    renameHonouringReadOnly,
    move,
    stop
};

// error is a Win32 code, ERROR_SUCCESS for a step that worked.
ReplaceStep nextReplaceStep(ReplaceStep tried, unsigned long error);

using ReplaceAttempt = std::function<std::error_code(ReplaceStep)>;
std::error_code replaceFileUsing(const ReplaceAttempt& attempt);

std::error_code replaceFileWithPosixSemantics(const std::filesystem::path& from,
                                              const std::filesystem::path& to,
                                              bool ignoringReadOnly = true);
std::error_code replaceFileByMoving(const std::filesystem::path& from,
                                    const std::filesystem::path& to);
#endif

} // namespace eacp::Detail

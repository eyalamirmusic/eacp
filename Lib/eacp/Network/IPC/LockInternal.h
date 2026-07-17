#pragma once

#include "Lock.h"

// The platform-specific file-locking backend, kept behind this header so the
// naming, path and polling logic in Lock.cpp stays platform-free. POSIX
// (macOS, iOS and Linux) and Win32 implementations live in Lock-Posix.cpp and
// Lock-Windows.cpp respectively.
namespace eacp::IPC::detail
{

// A native file handle held platform-agnostically: an int fd on POSIX, a
// HANDLE on Windows. Both compare equal to -1 when invalid once narrowed to
// intptr_t, so a single sentinel covers every platform.
using NativeFile = std::intptr_t;
inline constexpr NativeFile invalidFile = -1;

// Opens the lock file, creating it when absent, and takes nothing. The handle
// is deliberately not inherited by child processes: a child holding a copy
// would hold the lock too, and keep holding it after its parent died. Throws
// IPC::Error on failure.
NativeFile lockFileOpen(const FilePath& path);

// Takes the exclusive lock without blocking. False means another holder has
// it; throws IPC::Error when the attempt itself failed.
bool lockFileTryLock(NativeFile file);

// Drops the lock, leaving the handle open for a later lockFileTryLock. A
// no-op on invalidFile, so it is always safe to call.
void lockFileUnlock(NativeFile file) noexcept;

// Closes the handle, releasing any lock still held on it. A no-op on
// invalidFile.
void lockFileClose(NativeFile file) noexcept;

} // namespace eacp::IPC::detail

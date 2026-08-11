#pragma once

#include "Lock.h"

// The platform file-locking backend, implemented in Lock-Posix.cpp and
// Lock-Windows.cpp.
namespace eacp::IPC::detail
{

// An int fd on POSIX, a HANDLE on Windows; both narrow to -1 when invalid.
using NativeFile = std::intptr_t;
inline constexpr NativeFile invalidFile = -1;

// Opens the lock file, creating it when absent, and takes nothing. The handle
// is not inherited by child processes, which would otherwise hold the lock
// past their parent's death. Throws IPC::Error on failure.
NativeFile lockFileOpen(const FilePath& path);

// Takes the exclusive lock without blocking. False means another holder has
// it; throws IPC::Error when the attempt itself failed.
bool lockFileTryLock(NativeFile file);

// Drops the lock, leaving the handle open for a later lockFileTryLock. A
// no-op on invalidFile.
void lockFileUnlock(NativeFile file) noexcept;

// Closes the handle, releasing any lock still held on it. A no-op on
// invalidFile.
void lockFileClose(NativeFile file) noexcept;

} // namespace eacp::IPC::detail

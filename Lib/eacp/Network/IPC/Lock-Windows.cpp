#include "LockInternal.h"

#include <eacp/Core/Utils/WinInclude.h>

namespace eacp::IPC::detail
{
namespace
{
[[noreturn]] void throwLastError(const std::string& context)
{
    throw Error(context + ": Windows error " + std::to_string(::GetLastError()));
}
} // namespace

NativeFile lockFileOpen(const FilePath& path)
{
    // Sharing both ways is what lets a second holder open the file at all:
    // contention has to surface from LockFileEx, not as a sharing violation
    // here, or losing the race would throw instead of returning false. The
    // null security attributes leave the handle uninheritable, matching what
    // O_CLOEXEC buys on POSIX.
    auto handle = ::CreateFileW(path.wide().c_str(),
                                GENERIC_READ | GENERIC_WRITE,
                                FILE_SHARE_READ | FILE_SHARE_WRITE,
                                nullptr,
                                OPEN_ALWAYS,
                                FILE_ATTRIBUTE_NORMAL,
                                nullptr);

    if (handle == INVALID_HANDLE_VALUE)
        throwLastError("cannot open lock file '" + path.str() + "'");

    return (NativeFile) (std::intptr_t) handle;
}

bool lockFileTryLock(NativeFile file)
{
    auto overlapped = OVERLAPPED {};

    if (::LockFileEx((HANDLE) file,
                     LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY,
                     0,
                     MAXDWORD,
                     MAXDWORD,
                     &overlapped))
        return true;

    if (::GetLastError() == ERROR_LOCK_VIOLATION)
        return false;

    throwLastError("cannot take lock");
}

void lockFileUnlock(NativeFile file) noexcept
{
    if (file != invalidFile)
    {
        auto overlapped = OVERLAPPED {};
        ::UnlockFileEx((HANDLE) file, 0, MAXDWORD, MAXDWORD, &overlapped);
    }
}

void lockFileClose(NativeFile file) noexcept
{
    if (file != invalidFile)
        ::CloseHandle((HANDLE) file);
}

} // namespace eacp::IPC::detail

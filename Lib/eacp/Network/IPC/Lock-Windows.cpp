#include "LockInternal.h"

#include <eacp/Core/Utils/WinInclude.h>

namespace eacp::IPC::detail
{
namespace
{
std::wstring toWide(const std::string& text)
{
    if (text.empty())
        return {};

    auto length = ::MultiByteToWideChar(
        CP_UTF8, 0, text.c_str(), (int) text.size(), nullptr, 0);

    if (length <= 0)
        throw Error("cannot convert '" + text + "' to UTF-16");

    auto wide = std::wstring((std::size_t) length, L'\0');
    ::MultiByteToWideChar(
        CP_UTF8, 0, text.c_str(), (int) text.size(), wide.data(), length);
    return wide;
}

[[noreturn]] void throwLastError(const std::string& context)
{
    throw Error(context + ": Windows error " + std::to_string(::GetLastError()));
}
} // namespace

NativeFile lockFileOpen(const FilePath& path)
{
    // Sharing both ways lets a second holder open the file at all, so
    // contention surfaces from LockFileEx rather than as a throwing sharing
    // violation here. Null security attributes leave the handle uninheritable.
    auto handle = ::CreateFileW(toWide(path.str()).c_str(),
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

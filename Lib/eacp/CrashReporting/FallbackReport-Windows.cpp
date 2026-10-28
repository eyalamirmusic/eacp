#include "FallbackReport.h"

#include <eacp/Core/Utils/WinInclude.h>

namespace eacp::CrashReporting::Detail
{
CrashTimePath makeCrashTimePath(const FilePath& path)
{
    return path.wide();
}

bool createCrashTimeDirectory(const CrashTimePath& path)
{
    return ::CreateDirectoryW(path.c_str(), nullptr) != 0
           || ::GetLastError() == ERROR_ALREADY_EXISTS;
}

bool writeCrashTimeFile(const CrashTimePath& path, std::string_view bytes)
{
    auto* file = ::CreateFileW(path.c_str(),
                               GENERIC_WRITE,
                               0,
                               nullptr,
                               CREATE_ALWAYS,
                               FILE_ATTRIBUTE_NORMAL,
                               nullptr);

    if (file == INVALID_HANDLE_VALUE)
        return false;

    auto written = DWORD {0};
    auto size = static_cast<DWORD>(bytes.size());
    auto wrote = bytes.empty()
                 || ::WriteFile(file, bytes.data(), size, &written, nullptr) != 0;
    ::CloseHandle(file);

    return wrote && written == size;
}

bool renameCrashTimeFile(const CrashTimePath& from, const CrashTimePath& to)
{
    return ::MoveFileExW(from.c_str(), to.c_str(), MOVEFILE_REPLACE_EXISTING) != 0;
}
} // namespace eacp::CrashReporting::Detail

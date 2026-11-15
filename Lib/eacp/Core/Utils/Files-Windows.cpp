#include "Files.h"
#include "FilesPlatform.h"
#include "WinInclude.h"

#include "Logging.h"

#include <cstddef>
#include <cstring>
#include <memory>
#include <vector>

namespace eacp
{
namespace
{
std::error_code lastWindowsError()
{
    return {static_cast<int>(GetLastError()), std::system_category()};
}

// What MoveFileExW can get past: a system or filesystem without the POSIX
// rename (the first three) or one that ignores the read-only flag (access
// denied). Never a sharing violation, a holder without FILE_SHARE_DELETE.
bool movingCanGetPast(unsigned long error)
{
    return error == ERROR_INVALID_PARAMETER || error == ERROR_INVALID_FUNCTION
           || error == ERROR_NOT_SUPPORTED || error == ERROR_ACCESS_DENIED;
}

bool isReadOnly(DWORD attributes)
{
    return attributes != INVALID_FILE_ATTRIBUTES
           && (attributes & FILE_ATTRIBUTE_READONLY) != 0;
}

struct HandleCloser
{
    void operator()(HANDLE handle) const { CloseHandle(handle); }
};

using UniqueHandle = std::unique_ptr<void, HandleCloser>;

// FILE_RENAME_IGNORE_READONLY_ATTRIBUTE (Windows 10 1809): only the driver
// kit's ntifs.h defines it.
constexpr auto renameIgnoringReadOnly = DWORD {0x00000040};
} // namespace

namespace Files
{
FilePath executablePath()
{
    return Detail::modulePath(nullptr);
}

FilePath resourcesDirectory()
{
    return executablePath().parentDirectory();
}
} // namespace Files

namespace Detail
{
FilePath modulePath(void* module)
{
    // GetModuleFileNameW truncates instead of failing, so a full buffer means
    // try again — up to the longest path Windows accepts.
    for (auto size = std::size_t {MAX_PATH}; size <= 32768; size *= 2)
    {
        auto buffer = std::wstring(size, L'\0');
        auto length =
            GetModuleFileNameW((HMODULE) module, buffer.data(), (DWORD) size);

        if (length == 0)
            return {};

        if (length < size)
        {
            buffer.resize(length);
            return FilePath::fromWide(buffer);
        }
    }

    return {};
}

std::string bundleResourcePath(const std::string& /*filename*/)
{
    return {};
}

bool hasHiddenAttribute(const std::filesystem::path& path)
{
    const auto attributes = GetFileAttributesW(path.c_str());

    return attributes != INVALID_FILE_ATTRIBUTES
           && (attributes & FILE_ATTRIBUTE_HIDDEN) != 0;
}

// A handle open on the old file keeps reading it, as a descriptor does across
// rename(2), provided it was opened with FILE_SHARE_DELETE.
std::error_code replaceFileWithPosixSemantics(const std::filesystem::path& from,
                                              const std::filesystem::path& to,
                                              bool ignoringReadOnly)
{
    auto* source =
        CreateFileW(from.c_str(),
                    DELETE | SYNCHRONIZE,
                    FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                    nullptr,
                    OPEN_EXISTING,
                    FILE_ATTRIBUTE_NORMAL,
                    nullptr);

    if (source == INVALID_HANDLE_VALUE)
        return lastWindowsError();

    const auto handle = UniqueHandle {source};

    auto absoluteError = std::error_code {};
    const auto target = std::filesystem::absolute(to, absoluteError);

    if (absoluteError)
        return absoluteError;

    // SetFileInformationByHandle reads FileName as a NUL-terminated path,
    // whatever FileNameLength and the buffer size say, so the terminator is
    // copied with the name and the buffer has room for it.
    const auto& name = target.native();
    const auto nameBytes = name.size() * sizeof(wchar_t);
    const auto bufferBytes =
        offsetof(FILE_RENAME_INFO, FileName) + nameBytes + sizeof(wchar_t);
    auto storage = std::vector<FILE_RENAME_INFO>(
        (bufferBytes + sizeof(FILE_RENAME_INFO) - 1) / sizeof(FILE_RENAME_INFO));
    auto& info = storage.front();

    info.Flags = FILE_RENAME_FLAG_REPLACE_IF_EXISTS
                 | FILE_RENAME_FLAG_POSIX_SEMANTICS
                 | (ignoringReadOnly ? renameIgnoringReadOnly : DWORD {0});
    info.RootDirectory = nullptr;
    info.FileNameLength = static_cast<DWORD>(nameBytes);
    std::memcpy(info.FileName, name.c_str(), nameBytes + sizeof(wchar_t));

    if (SetFileInformationByHandle(
            handle.get(), FileRenameInfoEx, &info, static_cast<DWORD>(bufferBytes)))
        return {};

    return lastWindowsError();
}

// Not ReplaceFileW, which can fail with the destination renamed aside and the
// replacement not in its place, and not MOVEFILE_COPY_ALLOWED: a copy is not
// atomic. The read-only attribute is lifted for the move and put back if it
// fails; a successful move brings the replacement's own attributes.
std::error_code replaceFileByMoving(const std::filesystem::path& from,
                                    const std::filesystem::path& to)
{
    const auto attributes = GetFileAttributesW(to.c_str());
    const auto readOnly = isReadOnly(attributes);

    if (readOnly
        && !SetFileAttributesW(to.c_str(), attributes & ~FILE_ATTRIBUTE_READONLY))
        return lastWindowsError();

    if (MoveFileExW(from.c_str(), to.c_str(), MOVEFILE_REPLACE_EXISTING))
        return {};

    const auto error = lastWindowsError();

    if (readOnly && !SetFileAttributesW(to.c_str(), attributes))
        LOG("Files: '",
            FilePath {to}.str(),
            "' was left writable: restoring its read-only attribute failed"
            " with error ",
            static_cast<unsigned long>(GetLastError()));

    return error;
}

ReplaceStep nextReplaceStep(ReplaceStep tried, unsigned long error)
{
    if (error == ERROR_SUCCESS || tried == ReplaceStep::move
        || tried == ReplaceStep::stop)
        return ReplaceStep::stop;

    // Windows 10 1709 to 1803: the POSIX rename, but not the read-only flag.
    if (tried == ReplaceStep::renameIgnoringReadOnly
        && error == ERROR_INVALID_PARAMETER)
        return ReplaceStep::renameHonouringReadOnly;

    return movingCanGetPast(error) ? ReplaceStep::move : ReplaceStep::stop;
}

std::error_code replaceFileUsing(const ReplaceAttempt& attempt)
{
    auto error = std::error_code {};

    for (auto step = ReplaceStep::renameIgnoringReadOnly; step != ReplaceStep::stop;
         step = nextReplaceStep(step, static_cast<unsigned long>(error.value())))
        error = attempt(step);

    return error;
}

std::error_code replaceFile(const std::filesystem::path& from,
                            const std::filesystem::path& to)
{
    return replaceFileUsing(
        [&](ReplaceStep step)
        {
            if (step == ReplaceStep::move)
                return replaceFileByMoving(from, to);

            return replaceFileWithPosixSemantics(
                from, to, step == ReplaceStep::renameIgnoringReadOnly);
        });
}
} // namespace Detail
} // namespace eacp

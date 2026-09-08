#include "Files.h"
#include "FilesPlatform.h"
#include "WinInclude.h"

namespace eacp
{
namespace Files
{
FilePath resourcesDirectory()
{
    // GetModuleFileNameW truncates instead of failing, so a full buffer means
    // try again — up to the longest path Windows accepts.
    for (auto size = std::size_t {MAX_PATH}; size <= 32768; size *= 2)
    {
        auto buffer = std::wstring(size, L'\0');
        auto length = GetModuleFileNameW(nullptr, buffer.data(), (DWORD) size);

        if (length == 0)
            return {};

        if (length < size)
        {
            buffer.resize(length);
            return FilePath::fromWide(buffer).parentDirectory();
        }
    }

    return {};
}
} // namespace Files

namespace Detail
{
std::string bundleResourcePath(const std::string& /*filename*/)
{
    return {};
}
} // namespace Detail
} // namespace eacp

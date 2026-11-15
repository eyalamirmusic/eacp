#include "Files.h"
#include "FilesPlatform.h"

#include <filesystem>

namespace eacp
{
namespace Files
{
FilePath executablePath()
{
    auto ec = std::error_code {};
    auto executable = std::filesystem::read_symlink("/proc/self/exe", ec);

    if (ec)
        return {};

    return FilePath {executable};
}

FilePath resourcesDirectory()
{
    auto executable = executablePath();
    return executable.empty() ? FilePath {} : executable.parentDirectory();
}
} // namespace Files

namespace Detail
{
std::string bundleResourcePath(const std::string& /*filename*/)
{
    return {};
}

bool hasHiddenAttribute(const std::filesystem::path& /*path*/)
{
    return false;
}

std::error_code replaceFile(const std::filesystem::path& from,
                            const std::filesystem::path& to)
{
    auto error = std::error_code {};
    std::filesystem::rename(from, to, error);
    return error;
}
} // namespace Detail
} // namespace eacp

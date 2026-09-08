#include "Files.h"
#include "FilesPlatform.h"

#include <filesystem>

namespace eacp
{
namespace Files
{
FilePath resourcesDirectory()
{
    auto ec = std::error_code {};
    auto executable = std::filesystem::read_symlink("/proc/self/exe", ec);

    if (ec)
        return {};

    return FilePath {executable.parent_path()};
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

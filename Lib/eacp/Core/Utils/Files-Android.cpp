#include "Files.h"
#include "FilesPlatform.h"

#include "../Plugins/ModuleInfo.h"

#include <filesystem>

namespace eacp
{
namespace
{
std::filesystem::path processExecutable()
{
    auto ec = std::error_code {};
    auto executable = std::filesystem::read_symlink("/proc/self/exe", ec);

    return ec ? std::filesystem::path {} : executable;
}

bool isZygoteChild(const std::filesystem::path& executable)
{
    return executable.filename().string().starts_with("app_process");
}
} // namespace

namespace Files
{
FilePath executablePath()
{
    auto executable = processExecutable();

    if (executable.empty() || isZygoteChild(executable))
        return Plugins::getCurrentModulePath();

    return FilePath {executable};
}

// An app's resources are APK assets, which have no path: AAssetManager reads them.
FilePath resourcesDirectory()
{
    auto executable = processExecutable();

    if (executable.empty() || isZygoteChild(executable))
        return {};

    return FilePath {executable}.parentDirectory();
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
} // namespace Detail
} // namespace eacp

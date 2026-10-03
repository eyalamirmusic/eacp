#include "Files.h"
#include "FilesPlatform.h"

#include "../ObjC/CFRef.h"

#include <CoreFoundation/CoreFoundation.h>
#include <sys/stat.h>

namespace eacp
{
namespace
{
FilePath toFilePath(const CFRef<CFURLRef>& url)
{
    if (!url)
        return {};

    char path[1024] {};

    if (!CFURLGetFileSystemRepresentation(
            url, true, reinterpret_cast<UInt8*>(path), sizeof(path)))
        return {};

    return FilePath {path};
}
} // namespace

namespace Files
{
FilePath executablePath()
{
    return toFilePath(CFBundleCopyExecutableURL(CFBundleGetMainBundle()));
}

FilePath resourcesDirectory()
{
    return toFilePath(CFBundleCopyResourcesDirectoryURL(CFBundleGetMainBundle()));
}
} // namespace Files

namespace Detail
{
std::string bundleResourcePath(const std::string& filename)
{
    auto name = CFRef<CFStringRef> {CFStringCreateWithCString(
        nullptr, filename.c_str(), kCFStringEncodingUTF8)};

    return toFilePath(CFBundleCopyResourceURL(
                          CFBundleGetMainBundle(), name, nullptr, nullptr))
        .str();
}

// Finder's hidden flag, set with `chflags hidden`, which hides a file whose
// name carries no dot. lstat rather than stat: the flag belongs to the entry
// itself, as the dot does.
bool hasHiddenAttribute(const std::filesystem::path& path)
{
    struct stat info {};

    return lstat(path.c_str(), &info) == 0 && (info.st_flags & UF_HIDDEN) != 0;
}
} // namespace Detail
} // namespace eacp

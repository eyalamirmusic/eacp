#include "Files.h"
#include "FilesPlatform.h"

#include "../ObjC/CFRef.h"

#include <CoreFoundation/CoreFoundation.h>

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
} // namespace Detail
} // namespace eacp

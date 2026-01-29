#include "Files.h"
#include <CoreFoundation/CoreFoundation.h>

namespace eacp::Files
{
std::string getBundleResourcePath(const std::string& filename)
{
    auto ref = CFBundleGetMainBundle();
    auto name = CFStringCreateWithCString(
        nullptr, filename.c_str(), kCFStringEncodingUTF8);
    auto url = CFBundleCopyResourceURL(ref, name, nullptr, nullptr);
    CFRelease(name);

    if (url == nullptr)
        return {};

    char path[1024];
    CFURLGetFileSystemRepresentation(
        url, true, reinterpret_cast<UInt8*>(path), sizeof(path));
    CFRelease(url);
    return path;
}
} // namespace eacp::Files

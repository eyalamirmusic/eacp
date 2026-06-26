#import <AppKit/AppKit.h>

#include "Clipboard.h"

#include <eacp/Core/ObjC/Strings.h>

namespace TrayAppNative
{
bool copyFilesToClipboard(const EA::Vector<std::string>& paths)
{
    if (paths.empty())
        return false;

    auto* urls = [NSMutableArray arrayWithCapacity:paths.size()];

    for (const auto& path: paths)
    {
        auto* nsPath = eacp::Strings::toNSString(path);
        auto* url = [NSURL fileURLWithPath:nsPath];

        if (url == nil)
            continue;

        [urls addObject:url];
    }

    if (urls.count == 0)
        return false;

    auto* pasteboard = [NSPasteboard generalPasteboard];
    [pasteboard clearContents];

    return [pasteboard writeObjects:urls];
}
} // namespace TrayAppNative

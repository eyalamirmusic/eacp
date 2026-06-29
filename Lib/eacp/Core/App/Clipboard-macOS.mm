#import <AppKit/AppKit.h>

#include "Clipboard.h"

#include <eacp/Core/ObjC/Strings.h>

namespace eacp::Clipboard
{
bool copyText(std::string_view text)
{
    auto storage = std::string {text};
    auto* pasteboard = [NSPasteboard generalPasteboard];
    [pasteboard clearContents];
    return [pasteboard setString:Strings::toNSString(storage)
                         forType:NSPasteboardTypeString];
}

bool copyFiles(const Vector<std::string>& paths)
{
    if (paths.empty())
        return false;

    auto* urls = [NSMutableArray arrayWithCapacity:paths.size()];

    for (const auto& path: paths)
    {
        auto* nsPath = Strings::toNSString(path);
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
} // namespace eacp::Clipboard

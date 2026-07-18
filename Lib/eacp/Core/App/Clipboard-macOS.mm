#import <AppKit/AppKit.h>

#include "Clipboard.h"

#include "../ObjC/Strings.h"

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

std::string getText()
{
    auto* pasteboard = [NSPasteboard generalPasteboard];
    auto* text = [pasteboard stringForType:NSPasteboardTypeString];

    // nil when the pasteboard holds something that is not text, or nothing.
    if (text == nil)
        return {};

    return Strings::toStdString(text);
}

bool hasText()
{
    auto* pasteboard = [NSPasteboard generalPasteboard];

    // Asks what types are available rather than fetching the payload, so a
    // menu can enable Paste without copying a large clipboard.
    return [pasteboard availableTypeFromArray:@[NSPasteboardTypeString]] != nil;
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

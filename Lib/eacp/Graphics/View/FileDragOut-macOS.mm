#include "FileDragOut-macOS.h"
#include "../Helpers/ImageConversion-macOS.h"

#include <eacp/Core/ObjC/Strings.h>

namespace eacp::Graphics
{
namespace
{
NSImage* dragImageFor(const Image& image, float imageScale)
{
    auto* nsImage = toNSImage(image);

    if (nsImage == nil)
        return nil;

    auto scale = imageScale > 0.f ? (CGFloat) imageScale : 1.0;
    nsImage.size = NSMakeSize(image.width() / scale, image.height() / scale);
    return nsImage;
}
} // namespace

NSArray<NSDraggingItem*>* makeFileDraggingItems(NSView* view,
                                                NSPoint anchor,
                                                const Vector<std::string>& paths,
                                                const Image& image,
                                                float imageScale)
{
    constexpr CGFloat iconSize = 64.0;
    constexpr CGFloat stackOffset = 8.0;

    auto* items = [NSMutableArray arrayWithCapacity:paths.size()];

    if (view == nil)
        return items;

    auto* workspace = [NSWorkspace sharedWorkspace];
    auto* customImage = dragImageFor(image, imageScale);

    NSUInteger index = 0;

    for (const auto& path: paths)
    {
        auto* nsPath = Strings::toNSString(path);
        auto* fileURL = [NSURL fileURLWithPath:nsPath];

        if (fileURL == nil)
            continue;

        auto* item = [[NSDraggingItem alloc] initWithPasteboardWriter:fileURL];

        auto* contents = customImage;

        if (contents == nil)
        {
            contents = [workspace iconForFile:nsPath];
            contents.size = NSMakeSize(iconSize, iconSize);
        }

        auto size = contents.size;
        auto offset = (CGFloat) index * stackOffset;

        [item setDraggingFrame:NSMakeRect(anchor.x - size.width / 2 + offset,
                                          anchor.y - size.height / 2 - offset,
                                          size.width,
                                          size.height)
                      contents:contents];

        [items addObject:item];
        [item release];
        ++index;
    }

    return items;
}

FileDragResult fileDragResultFor(NSDragOperation operation)
{
    if (operation == NSDragOperationNone)
        return FileDragResult::Cancelled;

    if ((operation & NSDragOperationMove) != 0)
        return FileDragResult::Moved;

    return FileDragResult::Copied;
}

} // namespace eacp::Graphics

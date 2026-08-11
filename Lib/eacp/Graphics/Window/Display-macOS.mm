#include "Display.h"

#import <Cocoa/Cocoa.h>

namespace eacp::Graphics
{
namespace
{
// AppKit is bottom-left origin on the PRIMARY screen; eacp is top-left origin
// on the same screen. So the flip is about the primary screen's own frame, not
// the screen being converted.
Rect toScreenPoints(NSRect rect)
{
    const auto primaryTop = NSMaxY(NSScreen.screens.firstObject.frame);

    return {(float) NSMinX(rect),
            (float) (primaryTop - NSMaxY(rect)),
            (float) NSWidth(rect),
            (float) NSHeight(rect)};
}
} // namespace

Display primaryDisplay()
{
    // firstObject, not mainScreen: "main" holds the key window and moves with
    // the user, while the first carries the menu bar.
    NSScreen* screen = NSScreen.screens.firstObject;

    if (screen == nil)
        return {{0.f, 0.f, 1280.f, 800.f}, {0.f, 0.f, 1280.f, 800.f}, 1.f};

    return {toScreenPoints(screen.frame),
            toScreenPoints(screen.visibleFrame),
            (float) screen.backingScaleFactor};
}
} // namespace eacp::Graphics

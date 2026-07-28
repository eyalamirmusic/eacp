#include "Display.h"

#import <Cocoa/Cocoa.h>

namespace eacp::Graphics
{
namespace
{
// AppKit screen coordinates put the origin at the *primary* screen's
// bottom-left and grow upward; every eacp screen point is measured from the
// primary screen's top-left and grows downward, which is what
// WindowOptions::initialPosition already means. The flip is therefore about the
// primary screen's own frame, not about the screen being converted - otherwise
// a display sitting above or below the primary one would come back in the wrong
// place, which is exactly the multi-monitor case this exists to serve.
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
    // firstObject, not mainScreen: "main" is the screen holding the key window,
    // which moves with the user, while the first is the one carrying the menu
    // bar and the one every coordinate here is measured from.
    NSScreen* screen = NSScreen.screens.firstObject;

    if (screen == nil)
        return {{0.f, 0.f, 1280.f, 800.f}, {0.f, 0.f, 1280.f, 800.f}, 1.f};

    return {toScreenPoints(screen.frame),
            toScreenPoints(screen.visibleFrame),
            (float) screen.backingScaleFactor};
}
} // namespace eacp::Graphics

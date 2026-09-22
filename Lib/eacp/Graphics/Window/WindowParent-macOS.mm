#include "Window.h"

#import <Cocoa/Cocoa.h>

namespace eacp::Graphics::detail
{
namespace
{
NSWindow* nativeWindow(Window& window)
{
    return (NSWindow*) window.getHandle();
}
} // namespace

// addChildWindow:ordered:NSWindowAbove is the whole of a docked window on
// AppKit: the child sits directly above its parent and above nothing else,
// travels with it, orders out with it and miniaturises with it. Only the
// ownership is set here; where the child sits is WindowOptions::parentOffset
// and Window::setPosition.
//
// Nothing is allocated, so there is nothing to release: both windows are
// owned by their Window::Native (this file is MRC, like the rest of the
// Objective-C++ here).
void attachNativeParent(Window& child, Window* parent)
{
    auto* childWindow = nativeWindow(child);

    if (childWindow == nil)
        return;

    auto* parentWindow = parent != nullptr ? nativeWindow(*parent) : nil;

    if (childWindow.parentWindow == parentWindow)
        return;

    if (childWindow.parentWindow != nil)
        [childWindow.parentWindow removeChildWindow:childWindow];

    if (parentWindow != nil)
        [parentWindow addChildWindow:childWindow ordered:NSWindowAbove];
}

bool backendCarriesChildWindows()
{
    return true;
}
} // namespace eacp::Graphics::detail

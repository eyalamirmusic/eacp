#import <Cocoa/Cocoa.h>

#include "KeyForwarding-macOS.h"
#include "Window-macOS.h"
#include "../Graphics/Keyboard-MacOS.h"
#include "../View/View-MacOS.h"
#include "../View/View.h"

namespace eacp::Graphics
{

namespace
{
NSView* outermostFrameworkView(NSView* view)
{
    while (view.superview != nil && isFrameworkNativeView(view.superview))
        view = view.superview;

    return view;
}

void deliverThroughResponderChain(NSView* start, NSEvent* event, bool isDown)
{
    auto* next = outermostFrameworkView(start).nextResponder;

    if (next == nil)
        return;

    if (isDown)
        [next keyDown:event];
    else
        [next keyUp:event];
}

bool isHostMainWindowCandidate(NSWindow* window, NSWindow* panel, NSWindow* source)
{
    return window != nil && window != panel && window != source && window.isVisible
           && window.canBecomeMainWindow && ![window isKindOfClass:[NSPanel class]]
           && !isFrameworkWindow(window);
}

NSWindow* hostMainWindow(NSWindow* panel, NSWindow* source)
{
    if (isHostMainWindowCandidate(NSApp.mainWindow, panel, source))
        return NSApp.mainWindow;

    NSWindow* largest = nil;
    auto largestArea = 0.0;

    for (NSWindow* window in NSApp.orderedWindows)
    {
        if (!isHostMainWindowCandidate(window, panel, source))
            continue;

        auto area = window.frame.size.width * window.frame.size.height;

        if (area > largestArea)
        {
            largest = window;
            largestArea = area;
        }
    }

    return largest;
}

NSEvent* retargeted(NSEvent* event, NSWindow* target, bool isDown)
{
    return [NSEvent keyEventWithType:isDown ? NSEventTypeKeyDown : NSEventTypeKeyUp
                            location:NSZeroPoint
                       modifierFlags:event.modifierFlags
                           timestamp:event.timestamp
                        windowNumber:target.windowNumber
                             context:nil
                          characters:event.characters
         charactersIgnoringModifiers:event.charactersIgnoringModifiers
                           isARepeat:event.isARepeat
                             keyCode:event.keyCode];
}

// Hosts ignore a key unless the target is key. It must go through -[NSApplication sendEvent:],
// not the window or postEvent:, because menu key equivalents and app-level shortcut hooks
// (Reaper, Logic) only run there, and a posted event would be re-grabbed by KeyGrab's monitor.
void deliverWhileKey(NSWindow* target, NSEvent* event, bool isDown, NSWindow* source)
{
    auto* previous = NSApp.keyWindow != nil ? NSApp.keyWindow : source;

    [target makeKeyWindow];
    [NSApp sendEvent:retargeted(event, target, isDown)];

    if (previous != target)
        [previous makeKeyWindow];
}
} // namespace

NativeKeyEvent nativeKeyEventFrom(NSEvent* event)
{
    auto type =
        event.type == NSEventTypeKeyDown ? KeyEventType::Down : KeyEventType::Up;

    return {.key = keyEventFrom(event, type),
            .nativeKey = event.keyCode,
            .nsEvent = event};
}

void EmbedderKeyForwarder::deliver(const NativeKeyEvent& event)
{
    auto* nsEvent = (NSEvent*) event.nsEvent;
    auto* start = (NSView*) from.getHandle();

    if (nsEvent == nil || start == nil)
        return;

    auto isDown = event.key.type == KeyEventType::Down;
    auto* panel = start.window;
    auto* source = nsEvent.window;

    if (source != nil && source != panel)
    {
        if (auto* main = hostMainWindow(panel, source))
        {
            deliverWhileKey(main, nsEvent, isDown, source);
            return;
        }
    }

    deliverThroughResponderChain(start, nsEvent, isDown);
}

} // namespace eacp::Graphics

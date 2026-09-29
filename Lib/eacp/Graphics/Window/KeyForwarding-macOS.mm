#import <Cocoa/Cocoa.h>

#include "KeyForwarding-macOS.h"
#include "../Graphics/Keyboard-MacOS.h"
#include "../View/View-MacOS.h"

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

    auto* next = outermostFrameworkView(start).nextResponder;

    if (next == nil)
        return;

    if (event.key.type == KeyEventType::Down)
        [next keyDown:nsEvent];
    else
        [next keyUp:nsEvent];
}

} // namespace eacp::Graphics

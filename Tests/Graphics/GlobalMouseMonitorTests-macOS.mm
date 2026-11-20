#import <AppKit/AppKit.h>

#include "Common.h"

#include <eacp/Graphics/GlobalInput/GlobalMouseMonitor-macOS.h>

// What a global mouse monitor reports, from NSEvents built here and handed to
// its translation: nothing is posted to the system.

using namespace nano;
using namespace eacp::Graphics;

namespace
{
constexpr auto pressX = 120.0;
constexpr auto pressY = 40.0;
constexpr auto releaseX = 300.0;
constexpr auto releaseY = 90.0;
constexpr auto clicksInADoubleClick = 2;

// An event with no window, as a global monitor sees one. Built from a CGEvent,
// which measures from the primary screen's top-left as eacp does, so the
// translation's flip from AppKit's bottom-left origin is under test too.
NSEvent* makeMouseEvent(CGEventType type, CGMouseButton button, CGPoint position, int clicks)
{
    auto cgEvent = CGEventCreateMouseEvent(nullptr, type, position, button);
    CGEventSetIntegerValueField(cgEvent, kCGMouseEventClickState, clicks);

    auto* event = [NSEvent eventWithCGEvent:cgEvent];
    CFRelease(cgEvent);
    return event;
}
} // namespace

auto tPressIsInDesktopPoints = test("GlobalMouseMonitor/aPressIsInDesktopPoints") = []
{
    auto translator = GlobalMouseEventTranslator {};
    auto event = translator.translate(
        makeMouseEvent(kCGEventRightMouseDown, kCGMouseButtonRight, CGPointMake(pressX, pressY), clicksInADoubleClick));

    check(event.has_value());
    check(event->type == MouseEventType::Down);
    check(event->button == MouseButton::Right);
    check(event->clickCount == clicksInADoubleClick);
    check(event->pos.x == (float) pressX);
    check(event->pos.y == (float) pressY);
};

auto tReleaseKnowsWhereThePressWas =
    test("GlobalMouseMonitor/aReleaseKnowsWhereThePressWas") = []
{
    auto translator = GlobalMouseEventTranslator {};
    translator.translate(makeMouseEvent(kCGEventLeftMouseDown, kCGMouseButtonLeft, CGPointMake(pressX, pressY), 1));

    auto event =
        translator.translate(makeMouseEvent(kCGEventLeftMouseUp, kCGMouseButtonLeft, CGPointMake(releaseX, releaseY), 1));

    check(event.has_value());
    check(event->type == MouseEventType::Up);
    check(event->pos.x == (float) releaseX);
    check(event->downPos.x == (float) pressX);
    check(event->downPos.y == (float) pressY);
};

auto tKeyEventIsNotAMouseEvent = test("GlobalMouseMonitor/aKeyEventIsNotAMouseEvent") = []
{
    auto* keyEvent = [NSEvent keyEventWithType:NSEventTypeKeyDown
                                      location:NSZeroPoint
                                 modifierFlags:0
                                     timestamp:0
                                  windowNumber:0
                                       context:nil
                                    characters:@"a"
                   charactersIgnoringModifiers:@"a"
                                     isARepeat:NO
                                       keyCode:0];

    auto translator = GlobalMouseEventTranslator {};
    check(!translator.translate(keyEvent).has_value());
};

auto tPressesOnlyAsksForNoMoves = test("GlobalMouseMonitor/pressesOnlyAsksForNoMoves") = []
{
    auto mask = getEventMaskForTypes({MouseEventType::Down});

    check((mask & NSEventMaskLeftMouseDown) != 0);
    check((mask & NSEventMaskOtherMouseDown) != 0);
    check((mask & NSEventMaskMouseMoved) == 0);
    check((mask & NSEventMaskLeftMouseUp) == 0);
    check((mask & NSEventMaskScrollWheel) == 0);
};

auto tDragsAlsoAskForPresses = test("GlobalMouseMonitor/dragsAlsoAskForPresses") = []
{
    auto mask = getEventMaskForTypes({MouseEventType::Dragged});

    check((mask & NSEventMaskLeftMouseDragged) != 0);
    check((mask & NSEventMaskLeftMouseDown) != 0);
    check((mask & NSEventMaskMouseMoved) == 0);
};

auto tNothingAskedForMonitorsNothing =
    test("GlobalMouseMonitor/nothingAskedForMonitorsNothing") = []
{
    check(getEventMaskForTypes({}) == 0);
    check(getEventMaskForTypes({MouseEventType::Entered, MouseEventType::Exited}) == 0);
};

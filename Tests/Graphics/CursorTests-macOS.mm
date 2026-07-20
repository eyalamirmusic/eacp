#import <AppKit/AppKit.h>
#import <objc/runtime.h>

#include "Common.h"

// The platform half of cursor shapes. CursorTests.cpp covers the state; what
// cannot be reached from there is whether AppKit is ever told about it.
//
// Two things have to be true and neither is visible from portable code: the
// backing view must implement cursorUpdate: *itself*, and its tracking area must
// ask for cursor updates at all. Miss either and the shape is remembered
// perfectly, every portable test passes, and the pointer stays an arrow.

using namespace nano;
using namespace eacp::Graphics;

namespace
{
NSView* nativeViewOf(View& view)
{
    return (__bridge NSView*) view.nativeFocusTarget();
}

// eacp's cursorUpdate: ignores its event, so there is nothing to synthesise —
// but the selector is declared nonnull, and a literal nil is a warning.
void sendCursorUpdate(NSView* view)
{
    NSEvent* unusedEvent = nil;
    [view cursorUpdate:unusedEvent];
}
} // namespace

// The backing view implements cursorUpdate: rather than inheriting it.
//
// respondsToSelector: is worthless on its own here — NSView implements
// cursorUpdate: itself, so it answers YES whether or not eacp registered
// anything, and inheriting NSView's version is exactly the silent no-op that
// leaves the arrow in place. Same trap as scrollWheel:, so the same test shape:
// compare the resolved method against the immediate superclass's.
auto tNativeViewImplementsCursorUpdate =
    test("Cursor/nativeViewImplementsCursorUpdate") = []
{
    auto view = View {};
    auto* native = nativeViewOf(view);

    check(native != nil);
    check([native respondsToSelector:@selector(cursorUpdate:)]);

    auto* cls = [native class];
    auto* parent = class_getSuperclass(cls);

    check(parent != nil);

    auto* mine = class_getInstanceMethod(cls, @selector(cursorUpdate:));
    auto* inherited = class_getInstanceMethod(parent, @selector(cursorUpdate:));

    check(mine != nullptr);
    check(mine != inherited);
};

// The tracking area asks for cursor updates. Without the flag AppKit never
// sends cursorUpdate: at all, so the method above would be registered and never
// called — and nothing else about the view would look wrong.
auto tTrackingAreaAsksForCursorUpdates =
    test("Cursor/trackingAreaAsksForCursorUpdates") = []
{
    auto view = View {};
    auto* native = nativeViewOf(view);

    check(native != nil);

    // Tracking areas are rebuilt on demand rather than at construction.
    [native updateTrackingAreas];

    check(native.trackingAreas.count > 0);

    auto found = false;

    for (NSTrackingArea* area in native.trackingAreas)
        if ((area.options & NSTrackingCursorUpdate) != 0)
            found = true;

    check(found);

    // The wheel and move tracking this view already relied on is still there:
    // rebuilding the options list is the easy way to drop one by accident.
    auto keptMouseMoved = false;

    for (NSTrackingArea* area in native.trackingAreas)
        if ((area.options & NSTrackingMouseMoved) != 0)
            keptMouseMoved = true;

    check(keptMouseMoved);
};

// cursorUpdate: sets the shape the view is carrying, which is the whole point
// of the method — and this is the end-to-end check of the enum-to-NSCursor
// mapping, since NSCursor.currentCursor reports what was last set.
auto tCursorUpdateAppliesTheShape = test("Cursor/cursorUpdateAppliesTheShape") = []
{
    auto view = View {};
    auto* native = nativeViewOf(view);

    check(native != nil);

    view.setMouseCursor(MouseCursor::IBeam);
    sendCursorUpdate(native);

    check([NSCursor currentCursor] == [NSCursor IBeamCursor]);

    view.setMouseCursor(MouseCursor::ResizeLeftRight);
    sendCursorUpdate(native);

    check([NSCursor currentCursor] == [NSCursor resizeLeftRightCursor]);

    // And back to the arrow, so Default is a real shape rather than "leave
    // whatever was there".
    view.setMouseCursor(MouseCursor::Default);
    sendCursorUpdate(native);

    check([NSCursor currentCursor] == [NSCursor arrowCursor]);
};

// No two shapes map onto the same NSCursor. A switch that fell through would
// collapse two of them silently, and the only symptom is the wrong pointer over
// one particular region.
auto tShapesMapToDistinctCursors = test("Cursor/shapesMapToDistinctCursors") = []
{
    auto view = View {};
    auto* native = nativeViewOf(view);

    check(native != nil);

    const auto shapes = {MouseCursor::Default,
                         MouseCursor::IBeam,
                         MouseCursor::PointingHand,
                         MouseCursor::ResizeLeftRight,
                         MouseCursor::ResizeUpDown,
                         MouseCursor::Crosshair};

    auto* seen = [NSMutableArray<NSCursor*> array];

    for (auto shape: shapes)
    {
        view.setMouseCursor(shape);
        sendCursorUpdate(native);

        auto* current = [NSCursor currentCursor];

        check(current != nil);
        check(![seen containsObject:current]);

        [seen addObject:current];
    }

    check(seen.count == 6);
};

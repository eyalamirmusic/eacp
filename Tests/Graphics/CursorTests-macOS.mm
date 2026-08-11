#import <AppKit/AppKit.h>
#import <objc/runtime.h>

#include "Common.h"

using namespace nano;
using namespace eacp::Graphics;

namespace
{
NSView* nativeViewOf(View& view)
{
    return (__bridge NSView*) view.nativeFocusTarget();
}

// cursorUpdate: ignores its event, but the selector is declared nonnull and a
// literal nil warns.
void sendCursorUpdate(NSView* view)
{
    NSEvent* unusedEvent = nil;
    [view cursorUpdate:unusedEvent];
}
} // namespace

// NSView implements cursorUpdate: itself, so respondsToSelector: answers YES
// either way; the resolved method has to differ from the superclass's.
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

    auto keptMouseMoved = false;

    for (NSTrackingArea* area in native.trackingAreas)
        if ((area.options & NSTrackingMouseMoved) != 0)
            keptMouseMoved = true;

    check(keptMouseMoved);
};

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

    view.setMouseCursor(MouseCursor::Default);
    sendCursorUpdate(native);

    check([NSCursor currentCursor] == [NSCursor arrowCursor]);
};

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

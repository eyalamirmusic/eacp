#include "Common.h"

// The portable half of cursor shapes: the state every backend reads. Whether
// AppKit is ever *told* is in CursorTests-macOS.mm.
//
// Worth having both, because the two halves fail differently — this one catches
// a view that forgets what it was set to, and the macOS one catches a view that
// remembers perfectly and never draws it.

using namespace nano;
using namespace eacp::Graphics;

// A view starts with the arrow, which is the case every view that never
// mentions a cursor is in.
auto tDefaultIsTheArrow = test("Cursor/defaultIsTheArrow") = []
{
    const auto view = View {};

    check(view.getMouseCursor() == MouseCursor::Default);
};

auto tShapeIsRemembered = test("Cursor/shapeIsRemembered") = []
{
    auto view = View {};

    view.setMouseCursor(MouseCursor::ResizeLeftRight);
    check(view.getMouseCursor() == MouseCursor::ResizeLeftRight);

    view.setMouseCursor(MouseCursor::IBeam);
    check(view.getMouseCursor() == MouseCursor::IBeam);

    view.setMouseCursor(MouseCursor::Default);
    check(view.getMouseCursor() == MouseCursor::Default);
};

// Setting the same shape twice is free, so a mouseMoved handler can call this
// on every move without checking first — which is exactly how a splitter uses
// it, and the reason the setter early-outs rather than the caller.
auto tSettingTheSameShapeTwiceIsFine =
    test("Cursor/settingTheSameShapeTwiceIsFine") = []
{
    auto view = View {};

    for (auto repeat = 0; repeat < 5; ++repeat)
        view.setMouseCursor(MouseCursor::ResizeUpDown);

    check(view.getMouseCursor() == MouseCursor::ResizeUpDown);
};

// Each view carries its own, so one region changing shape does not drag the
// rest of the window with it.
auto tShapeIsPerView = test("Cursor/shapeIsPerView") = []
{
    auto first = View {};
    auto second = View {};

    first.setMouseCursor(MouseCursor::Crosshair);

    check(first.getMouseCursor() == MouseCursor::Crosshair);
    check(second.getMouseCursor() == MouseCursor::Default);
};

// Settable before the view is ever on screen. A widget layer decides its shapes
// while laying out, which can happen before the window exists, and a setter that
// needed a live window would either crash or silently drop that.
auto tSettableBeforeTheViewIsShown = test("Cursor/settableBeforeTheViewIsShown") = []
{
    auto view = View {};

    view.setMouseCursor(MouseCursor::PointingHand);

    check(view.getMouseCursor() == MouseCursor::PointingHand);
};

// Every shape round-trips. A switch that fell through would map two of these
// onto one, which is invisible until someone looks at the wrong pointer.
auto tEveryShapeRoundTrips = test("Cursor/everyShapeRoundTrips") = []
{
    auto view = View {};

    const auto shapes = {MouseCursor::Default,
                         MouseCursor::IBeam,
                         MouseCursor::PointingHand,
                         MouseCursor::ResizeLeftRight,
                         MouseCursor::ResizeUpDown,
                         MouseCursor::Crosshair};

    for (auto shape: shapes)
    {
        view.setMouseCursor(shape);
        check(view.getMouseCursor() == shape);
    }
};

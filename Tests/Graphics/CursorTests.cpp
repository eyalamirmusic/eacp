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

namespace
{
Image checkerImage(int size)
{
    auto image = Image {size, size};

    for (auto y = 0; y < size; ++y)
        for (auto x = 0; x < size; ++x)
            image.set(x,
                      y,
                      (x + y) % 2 == 0 ? Color {1.f, 0.f, 0.f, 1.f}
                                       : Color {0.f, 0.f, 1.f, 0.5f});

    return image;
}
} // namespace

// An image cursor keeps what it was made with, and is not one of the shapes.
auto tImageCursorCarriesItsGeometry =
    test("Cursor/anImageCursorCarriesItsHotspotAndScale") = []
{
    const auto image = checkerImage(32);
    const auto cursor = MouseCursor {image, {2.f, 5.f}, 4.f};

    check(cursor.isImage());
    check(cursor.getShape() == MouseCursor::Default);
    check(cursor.getHotspot().x == 2.f);
    check(cursor.getHotspot().y == 5.f);
    check(cursor.getScale() == 4.f);
    check(cursor.getImage().width() == 32);
    check(cursor.getImage() == image);
    check(!(cursor == MouseCursor::Default));
};

// Nothing to draw is the arrow, rather than an invisible pointer.
auto tInvalidImageIsTheArrow = test("Cursor/anInvalidImageIsTheArrow") = []
{
    const auto cursor = MouseCursor {Image {}, {0.f, 0.f}};

    check(!cursor.isImage());
    check(cursor == MouseCursor::Default);
};

// Two cursors made from the same pixels are the same cursor, which is what lets
// a host skip re-applying one a component sets on every move.
auto tImageCursorsCompareByIdentity =
    test("Cursor/imageCursorsCompareByTheirPixelsIdentity") = []
{
    const auto image = checkerImage(8);
    const auto cursor = MouseCursor {image, {1.f, 1.f}, 2.f};
    const auto copy = cursor;

    check(copy == cursor);
    check(MouseCursor {image, {1.f, 1.f}, 2.f} == cursor);

    check(!(MouseCursor {image, {2.f, 1.f}, 2.f} == cursor), "another hotspot");
    check(!(MouseCursor {image, {1.f, 1.f}, 1.f} == cursor), "another scale");

    auto samePixelsElsewhere = Image {8, 8, image.pixels()};
    check(!(MouseCursor {samePixelsElsewhere, {1.f, 1.f}, 2.f} == cursor),
          "equal pixels in another buffer are another cursor");
};

// The cursor holds its own copy: a later write to the caller's image detaches
// that image, so the cursor already made keeps its pixels and a cursor made
// after the write is a different one.
auto tWritingTheSourceDoesNotReachTheCursor =
    test("Cursor/writingTheSourceImageLeavesTheCursorAlone") = []
{
    auto image = checkerImage(8);
    const auto before = MouseCursor {image, {0.f, 0.f}};
    const auto original = before.getImage().at(0, 0);

    image.set(0, 0, Color {0.f, 1.f, 0.f, 1.f});

    check(before.getImage().at(0, 0).r == original.r);
    check(before.getImage().at(0, 0).g == original.g);
    check(!(MouseCursor {image, {0.f, 0.f}} == before));
};

auto tViewRemembersAnImageCursor = test("Cursor/aViewRemembersAnImageCursor") = []
{
    auto view = View {};
    const auto cursor = MouseCursor {checkerImage(16), {5.f, 5.f}};

    view.setMouseCursor(cursor);

    check(view.getMouseCursor() == cursor);
    check(view.getMouseCursor().getHotspot().x == 5.f);

    view.setMouseCursor(MouseCursor::IBeam);
    check(view.getMouseCursor() == MouseCursor::IBeam);
};

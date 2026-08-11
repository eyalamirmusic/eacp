#include "Common.h"

using namespace nano;
using namespace eacp::Graphics;

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

auto tSettingTheSameShapeTwiceIsFine =
    test("Cursor/settingTheSameShapeTwiceIsFine") = []
{
    auto view = View {};

    for (auto repeat = 0; repeat < 5; ++repeat)
        view.setMouseCursor(MouseCursor::ResizeUpDown);

    check(view.getMouseCursor() == MouseCursor::ResizeUpDown);
};

auto tShapeIsPerView = test("Cursor/shapeIsPerView") = []
{
    auto first = View {};
    auto second = View {};

    first.setMouseCursor(MouseCursor::Crosshair);

    check(first.getMouseCursor() == MouseCursor::Crosshair);
    check(second.getMouseCursor() == MouseCursor::Default);
};

auto tSettableBeforeTheViewIsShown = test("Cursor/settableBeforeTheViewIsShown") = []
{
    auto view = View {};

    view.setMouseCursor(MouseCursor::PointingHand);

    check(view.getMouseCursor() == MouseCursor::PointingHand);
};

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

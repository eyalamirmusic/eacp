#include "Common.h"

#include <eacp/Graphics/Window/PointerTrackers.h>

// The protocol-free pointer state machines the Linux seats and the web's DOM
// events share: the click count and its slop, and a frame's worth of scroll.

using namespace nano;
using namespace eacp::Graphics;

auto tPointerQuickClicksCount =
    test("PointerTracker/quickClicksInOneSpotCountUp") = []
{
    auto tracker = PointerTracker {};
    tracker.setPosition({10.f, 10.f});

    check(tracker.pressed(MouseButton::Left, 1000) == 1);
    tracker.released();
    check(!tracker.isButtonHeld());

    tracker.setPosition({12.f, 11.f});
    check(tracker.pressed(MouseButton::Left, 1200) == 2);
    check(tracker.isButtonHeld());
    check(same(tracker.getDownPosition(), {12.f, 11.f}));
};

auto tPointerSlowOrFarClicksRestart =
    test("PointerTracker/aSlowFarOrOtherButtonClickStartsAgain") = []
{
    auto tracker = PointerTracker {};

    check(tracker.pressed(MouseButton::Left, 1000) == 1);
    tracker.released();
    check(tracker.pressed(MouseButton::Left, 1600) == 1);
    tracker.released();

    tracker.setPosition({40.f, 0.f});
    check(tracker.pressed(MouseButton::Left, 1700) == 1);
    tracker.released();

    check(tracker.pressed(MouseButton::Right, 1800) == 1);
    check(tracker.getHeldButton() == MouseButton::Right);
};

auto tWheelPreciseIsPoints = test("WheelTracker/aPreciseFrameAddsUpPoints") = []
{
    auto wheel = WheelTracker {};
    wheel.setSource(true, false);
    wheel.addDelta({0.f, 3.f});
    wheel.addDelta({1.f, 2.f});

    check(wheel.isPending());
    check(same(wheel.getDelta(), {1.f, 5.f}));

    wheel.endFrame();

    check(!wheel.isPending());
    check(wheel.isPrecise());
    check(same(wheel.getDelta(), {0.f, 0.f}));
};

auto tWheelNotchesWin = test("WheelTracker/aNotchedFrameReportsLines") = []
{
    auto wheel = WheelTracker {};
    wheel.setSource(false, false);
    wheel.addDelta({0.f, 30.f});
    wheel.addNotches({0.f, 1.f});

    check(same(wheel.getDelta(), {0.f, 1.f}));
};

auto tCursorApplyOnlyOnChange =
    test("CursorTracker/onlyAChangeAsksForTheCursorAgain") = []
{
    auto cursor = CursorTracker {};

    check(!cursor.setShape(MouseCursor::Default));
    check(cursor.setShape(MouseCursor::IBeam));
    check(!cursor.setShape(MouseCursor::IBeam));
    check(cursor.setHidden(true));
    check(cursor.setShape(MouseCursor::IBeam));
    check(!cursor.isHidden());
};

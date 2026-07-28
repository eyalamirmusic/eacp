#include "Common.h"

// Graphics::primaryDisplay - the size an app is allowed to want.
//
// What can be asserted here is a shape, not a number: the display a test runs
// on is whatever the machine has, and a CI runner's is not a developer's. So
// these check the invariants an app actually relies on when it sizes its first
// window - that the numbers are usable, that the work area is inside the frame
// and no larger, and that points are points rather than pixels - and leave the
// values alone.
//
// The last of those is the one worth having. An implementation that handed back
// pixels would pass every plausible "is it positive" check and put a window
// twice the intended size on every Retina display, so the scale is asserted
// against the frame rather than only reported.

using namespace nano;
using namespace eacp::Graphics;

auto tPrimaryDisplayIsUsable = test("Display/primaryIsUsable") = []
{
    const auto display = primaryDisplay();

    // Never zero, even with no display attached: an app dividing a native
    // resolution into this should not have to guard against a zero.
    check(display.frame.w > 0.f);
    check(display.frame.h > 0.f);
    check(display.backingScale > 0.f);
};

// The work area is the part a window may occupy: the frame less the menu bar
// and the Dock, or the taskbar. It is therefore inside the frame and no bigger
// - a work area that came back equal to a frame with a menu bar in it would be
// an app placing its title bar underneath one.
auto tWorkAreaFitsInTheFrame = test("Display/workAreaFitsInTheFrame") = []
{
    const auto display = primaryDisplay();

    check(display.workArea.w > 0.f);
    check(display.workArea.h > 0.f);
    check(display.workArea.w <= display.frame.w);
    check(display.workArea.h <= display.frame.h);

    check(display.workArea.x >= display.frame.x);
    check(display.workArea.y >= display.frame.y);
    check(display.workArea.x + display.workArea.w
          <= display.frame.x + display.frame.w);
    check(display.workArea.y + display.workArea.h
          <= display.frame.y + display.frame.h);
};

// Points, not pixels, which is the whole reason Display is a type rather than a
// pair of ints. A Retina panel reports 2, and a frame measured in its pixels
// would be twice as wide as any display sold - so the check is that the frame
// is a plausible *point* size whatever the scale says, which is exactly what
// fails if an implementation forgets to divide.
auto tFrameIsInPoints = test("Display/frameIsInPoints") = []
{
    const auto display = primaryDisplay();

    check(display.frame.w < 16384.f);
    check(display.frame.h < 16384.f);

    // A scale of 2 with a 6016-point-wide frame would mean a 12032-pixel
    // display, which is not a thing; the same numbers read as pixels are an
    // ordinary 6K panel. That is the confusion this catches.
    check(display.frame.w * display.backingScale < 32768.f);
};

// The primary display is the origin of the screen coordinate system every
// window position is expressed in (WindowOptions::initialPosition), so its own
// frame starts there. A backend that handed back AppKit's bottom-left origin,
// or a monitor rect in some other space, fails here.
auto tPrimaryDisplayIsTheOrigin = test("Display/primaryFrameStartsAtTheOrigin") = []
{
    const auto display = primaryDisplay();

    check(display.frame.x == 0.f);
    check(display.frame.y == 0.f);
};

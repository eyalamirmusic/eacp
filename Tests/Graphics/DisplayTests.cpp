#include "Common.h"

using namespace nano;
using namespace eacp::Graphics;

auto tPrimaryDisplayIsUsable = test("Display/primaryIsUsable") = []
{
    const auto display = primaryDisplay();

    check(display.frame.w > 0.f);
    check(display.frame.h > 0.f);
    check(display.backingScale > 0.f);
};

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

auto tFrameIsInPoints = test("Display/frameIsInPoints") = []
{
    const auto display = primaryDisplay();

    check(display.frame.w < 16384.f);
    check(display.frame.h < 16384.f);

    // A 6016-point frame at scale 2 would be a 12032-pixel display, which does
    // not exist; the same numbers read as pixels are an ordinary 6K panel.
    check(display.frame.w * display.backingScale < 32768.f);
};

auto tPrimaryDisplayIsTheOrigin = test("Display/primaryFrameStartsAtTheOrigin") = []
{
    const auto display = primaryDisplay();

    check(display.frame.x == 0.f);
    check(display.frame.y == 0.f);
};

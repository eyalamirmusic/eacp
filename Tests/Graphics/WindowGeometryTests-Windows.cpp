// The placement rules a Win32 surface enforces for itself, tested as the pure
// geometry they are — no HWND, no display, no message loop. The window proc
// that uses them does nothing but read the frame, the work area and the DPI off
// the system and hand them over (Window-Windows.cpp), and the same is true of
// the child window an EmbeddedView places inside a host's window.

#include <eacp/Graphics/Window/WindowGeometry-Windows.h>
#include <NanoTest/NanoTest.h>

using namespace nano;
using namespace eacp::Graphics::detail;

namespace
{
// A 1920x1080 display with a 40px taskbar along the bottom.
constexpr auto workArea = RECT {0, 0, 1920, 1040};

RECT frameOf(LONG x, LONG y, LONG width, LONG height)
{
    return {x, y, x + width, y + height};
}

LONG widthOf(const RECT& r)
{
    return r.right - r.left;
}

LONG heightOf(const RECT& r)
{
    return r.bottom - r.top;
}
} // namespace

auto tContainLeavesFittingWindowAlone =
    test("WindowGeometry/containLeavesFittingWindowAlone") = []
{
    auto frame = frameOf(200, 100, 800, 600);
    containWithinWorkArea(frame, workArea, false);

    check(frame.left == 200);
    check(frame.top == 100);
    check(widthOf(frame) == 800);
    check(heightOf(frame) == 600);
};

// The reported bug: 1200x800 points at 200% is 2400x1600 physical, which is
// taller than the screen — so the bottom right, and with it the resize corner
// that is the only way back, opens off the display.
auto tContainTrimsWindowBiggerThanTheDisplay =
    test("WindowGeometry/containTrimsWindowBiggerThanTheDisplay") = []
{
    auto frame = frameOf(-240, -280, 2400, 1600);
    containWithinWorkArea(frame, workArea, false);

    check(widthOf(frame) == 1920);
    check(heightOf(frame) == 1040);
    check(frame.left == 0);
    check(frame.top == 0);
    check(frame.right == 1920);
    check(frame.bottom == 1040);
};

auto tContainSlidesOffscreenWindowBack =
    test("WindowGeometry/containSlidesOffscreenWindowBack") = []
{
    auto frame = frameOf(1700, 900, 800, 600);
    containWithinWorkArea(frame, workArea, false);

    // Size untouched, moved just far enough to fit above the taskbar.
    check(widthOf(frame) == 800);
    check(heightOf(frame) == 600);
    check(frame.right == 1920);
    check(frame.bottom == 1040);
};

auto tContainRespectsNonZeroWorkAreaOrigin =
    test("WindowGeometry/containRespectsNonZeroWorkAreaOrigin") = []
{
    // A second monitor to the right of the primary one, taskbar at its top.
    constexpr auto secondary = RECT {1920, 40, 3200, 1080};

    auto frame = frameOf(1900, 0, 800, 600);
    containWithinWorkArea(frame, secondary, false);

    check(frame.left == 1920);
    check(frame.top == 40);
    check(widthOf(frame) == 800);
    check(heightOf(frame) == 600);
};

// Trimming the sides independently would hand a ratio-locked window the one
// shape it exists to refuse, so both give way by the same factor.
auto tContainKeepsAspectRatio = test("WindowGeometry/containKeepsAspectRatio") = []
{
    auto frame = frameOf(0, 0, 3200, 1800); // 16:9, too big for the work area
    containWithinWorkArea(frame, workArea, true);

    check(widthOf(frame) <= 1920);
    check(heightOf(frame) <= 1040);

    // Height is the binding constraint (1040/1800 < 1920/3200), so the shape
    // is preserved off it.
    check(heightOf(frame) == 1040);
    check(widthOf(frame) == static_cast<LONG>(3200.0 * (1040.0 / 1800.0)));
};

auto tContainIgnoresAspectRatioWhenItAlreadyFits =
    test("WindowGeometry/containIgnoresAspectRatioWhenItAlreadyFits") = []
{
    auto frame = frameOf(100, 100, 1600, 900);
    containWithinWorkArea(frame, workArea, true);

    check(widthOf(frame) == 1600);
    check(heightOf(frame) == 900);
};

auto tHitTestCentreIsContent = test("WindowGeometry/hitTestCentreIsContent") = []
{
    constexpr auto frame = RECT {100, 100, 900, 700};

    check(resizeBandHitTest(frame, POINT {500, 400}, 8) == HTCLIENT);
    check(resizeBandHitTest(frame, POINT {120, 400}, 8) == HTCLIENT);
};

// One pixel inside the window is the edge: this is the case DefWindowProc gets
// wrong on a frameless window, answering HTCLIENT and leaving it unresizable.
auto tHitTestEdges = test("WindowGeometry/hitTestEdges") = []
{
    constexpr auto frame = RECT {100, 100, 900, 700};

    check(resizeBandHitTest(frame, POINT {100, 400}, 8) == HTLEFT);
    check(resizeBandHitTest(frame, POINT {107, 400}, 8) == HTLEFT);
    check(resizeBandHitTest(frame, POINT {899, 400}, 8) == HTRIGHT);
    check(resizeBandHitTest(frame, POINT {500, 100}, 8) == HTTOP);
    check(resizeBandHitTest(frame, POINT {500, 699}, 8) == HTBOTTOM);
};

// The band is exclusive at the far edge: the window rect's right/bottom are one
// past the last pixel, so 108 is already content.
auto tHitTestBandIsExactlyBandWide =
    test("WindowGeometry/hitTestBandIsExactlyBandWide") = []
{
    constexpr auto frame = RECT {100, 100, 900, 700};

    check(resizeBandHitTest(frame, POINT {107, 400}, 8) == HTLEFT);
    check(resizeBandHitTest(frame, POINT {108, 400}, 8) == HTCLIENT);
    check(resizeBandHitTest(frame, POINT {892, 400}, 8) == HTRIGHT);
    check(resizeBandHitTest(frame, POINT {891, 400}, 8) == HTCLIENT);
};

auto tHitTestCorners = test("WindowGeometry/hitTestCorners") = []
{
    constexpr auto frame = RECT {100, 100, 900, 700};

    check(resizeBandHitTest(frame, POINT {102, 102}, 8) == HTTOPLEFT);
    check(resizeBandHitTest(frame, POINT {898, 102}, 8) == HTTOPRIGHT);
    check(resizeBandHitTest(frame, POINT {102, 698}, 8) == HTBOTTOMLEFT);
    check(resizeBandHitTest(frame, POINT {898, 698}, 8) == HTBOTTOMRIGHT);
};

// A corner reaches twice the band along both of its edges, so the diagonal grab
// is a target the user can hit — but no further, or the edges disappear.
auto tHitTestCornerReach = test("WindowGeometry/hitTestCornerReach") = []
{
    constexpr auto frame = RECT {100, 100, 900, 700};

    check(resizeBandHitTest(frame, POINT {115, 102}, 8) == HTTOPLEFT);
    check(resizeBandHitTest(frame, POINT {116, 102}, 8) == HTTOP);
    check(resizeBandHitTest(frame, POINT {102, 115}, 8) == HTTOPLEFT);
    check(resizeBandHitTest(frame, POINT {102, 116}, 8) == HTLEFT);
};

// The band scales with the display: at 200% every measurement doubles.
auto tHitTestBandScalesWithDpi = test("WindowGeometry/hitTestBandScalesWithDpi") = []
{
    constexpr auto frame = RECT {0, 0, 1600, 1200};

    check(resizeBandHitTest(frame, POINT {15, 600}, 16) == HTLEFT);
    check(resizeBandHitTest(frame, POINT {16, 600}, 16) == HTCLIENT);
};

// Points to the pixels a child window is placed in. At 100% the numbers go
// through unchanged, which is the case that hides every scaling bug.
auto tPhysicalPixelsAtUnitScale =
    test("WindowGeometry/physicalPixelsAtUnitScale") = []
{
    auto pixels =
        toPhysicalPixels(eacp::Graphics::Rect {20.f, 40.f, 100.f, 60.f}, 1.f);

    check(pixels.left == 20);
    check(pixels.top == 40);
    check(widthOf(pixels) == 100);
    check(heightOf(pixels) == 60);
};

auto tPhysicalPixelsScalePosition =
    test("WindowGeometry/physicalPixelsScalePosition") = []
{
    auto pixels =
        toPhysicalPixels(eacp::Graphics::Rect {20.f, 40.f, 100.f, 60.f}, 1.5f);

    // The origin scales as well as the size: a surface placed 20 points into a
    // 150% host is 30 pixels in, and one that scaled only its size would drift
    // further from where the host put it the further across the window it sat.
    check(pixels.left == 30);
    check(pixels.top == 60);
    check(widthOf(pixels) == 150);
    check(heightOf(pixels) == 90);
};

// The rounding rule: outwards to whole pixels, never to the nearest. A surface
// that lands half a pixel short leaves a line of the host's own window showing
// down its right edge, and that seam is visible in a way half a pixel of
// overlap is not.
auto tPhysicalPixelsGrowToContainThePoints =
    test("WindowGeometry/physicalPixelsGrowToContainThePoints") = []
{
    auto pixels =
        toPhysicalPixels(eacp::Graphics::Rect {10.f, 10.f, 101.f, 101.f}, 1.5f);

    check(pixels.left == 15);
    check(pixels.top == 15);

    // 10 + 101 points at 150% is 166.5 pixels, and the surface has to cover it.
    check(pixels.right == 167);
    check(pixels.bottom == 167);
};

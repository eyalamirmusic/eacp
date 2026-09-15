#include "Common.h"

// The shape rule a window enforces, tested as the pure function it is. Every
// platform hands it a proposed size and applies what comes back, so what is
// worth checking is the arithmetic - which side gives way, how a fixed
// border is kept out of the ratio, and how a bound (a maximise, a display)
// is fitted - and none of it needs a window.

using namespace nano;
using eacp::Graphics::AspectRatioLock;
using eacp::Graphics::fitWithin;
using eacp::Graphics::Insets;
using eacp::Graphics::Point;
using eacp::Graphics::Rect;
using eacp::Graphics::ResizeAxis;
using eacp::Graphics::resizeAxisBetween;
using eacp::Graphics::ResizeRequest;
using eacp::Graphics::SizeConstraint;
using eacp::Graphics::WindowOptions;

namespace
{
bool same(float a, float b)
{
    return std::abs(a - b) < 0.0001f;
}

bool same(Point point, Point expected)
{
    return same(point.x, expected.x) && same(point.y, expected.y);
}

bool same(const Rect& rect, const Rect& expected)
{
    return same(rect.x, expected.x) && same(rect.y, expected.y)
           && same(rect.w, expected.w) && same(rect.h, expected.h);
}

const auto sixteenNine = Point {16.f, 9.f};
} // namespace

auto tDefaultAcceptsEverySize = test("SizeConstraint/defaultAcceptsEverySize") = []
{
    auto options = WindowOptions {};

    check(same(options.sizeConstraint({{123.f, 456.f}, ResizeAxis::Width}),
               {123.f, 456.f}));
    check(same(options.effectiveInitialSize(),
               {(float) options.width, (float) options.height}));
};

// A vertical edge sets the width and the height follows; a horizontal edge the
// reverse; a corner is driven by its width.
auto tLockFollowsTheDraggedEdge =
    test("SizeConstraint/lockFollowsTheDraggedEdge") = []
{
    auto lock = AspectRatioLock {sixteenNine};

    check(same(lock({{800.f, 100.f}, ResizeAxis::Width}), {800.f, 450.f}));
    check(same(lock({{100.f, 450.f}, ResizeAxis::Height}), {800.f, 450.f}));
    check(same(lock({{800.f, 100.f}, ResizeAxis::Both}), {800.f, 450.f}));
};

// The border keeps its thickness whatever the window does: only what is left
// after it is held to the ratio.
auto tLockKeepsTheBorderOutOfTheRatio =
    test("SizeConstraint/lockKeepsTheBorderOutOfTheRatio") = []
{
    auto lock = AspectRatioLock {sixteenNine, Insets {.top = 60.f, .right = 200.f}};

    check(same(lock({{1160.f, 0.f}, ResizeAxis::Width}), {1160.f, 600.f}));
    check(same(lock({{0.f, 600.f}, ResizeAxis::Height}), {1160.f, 600.f}));

    // Already allowed: handed back unchanged.
    check(same(lock({{1160.f, 600.f}, ResizeAxis::Both}), {1160.f, 600.f}));
};

// A size with no room for the locked content still leaves the border whole
// and a sliver of content rather than a negative one.
auto tLockNeverGoesBelowTheBorder =
    test("SizeConstraint/lockNeverGoesBelowTheBorder") = []
{
    auto lock = AspectRatioLock {sixteenNine, Insets {.top = 60.f}};
    auto allowed = lock({{0.f, 0.f}, ResizeAxis::Width});

    check(allowed.x >= 1.f);
    check(allowed.y >= 60.f);
};

// A ratio with a non-positive side describes no shape, so it constrains
// nothing rather than dividing by it.
auto tDegenerateRatioLocksNothing =
    test("SizeConstraint/degenerateRatioLocksNothing") = []
{
    for (auto ratio: {Point {0.f, 0.f}, Point {16.f, 0.f}, Point {-16.f, 9.f}})
    {
        auto lock = AspectRatioLock {ratio, Insets {.top = 60.f}};

        check(!lock.isLocking());
        check(same(lock({{777.f, 333.f}, ResizeAxis::Width}), {777.f, 333.f}));
    }

    check(AspectRatioLock {{1920.f, 1080.f}}.isLocking());
};

// The older fields are the same rule spelled shorter: aspectRatio is a
// borderless lock, onWillResize a clamp that runs first. Both keep working
// beside sizeConstraint, and the lock has the last word.
auto tAspectRatioFieldIsTheLock =
    test("SizeConstraint/aspectRatioFieldIsTheLock") = []
{
    auto options = WindowOptions {};
    options.aspectRatio = sixteenNine;

    check(
        same(options.effectiveSizeConstraint()({{800.f, 100.f}, ResizeAxis::Width}),
             {800.f, 450.f}));
};

auto tDegenerateAspectRatioFieldLocksNothing =
    test("SizeConstraint/degenerateAspectRatioFieldLocksNothing") = []
{
    auto options = WindowOptions {};

    for (auto ratio: {Point {0.f, 0.f}, Point {16.f, 0.f}, Point {-16.f, 9.f}})
    {
        options.aspectRatio = ratio;
        check(!options.hasAspectRatio());
        check(same(
            options.effectiveSizeConstraint()({{777.f, 333.f}, ResizeAxis::Width}),
            {777.f, 333.f}));
    }

    options.aspectRatio = Point {1920.f, 1080.f};
    check(options.hasAspectRatio());
};

auto tWillResizeRunsFirstAndTheLockLast =
    test("SizeConstraint/willResizeRunsFirstAndTheLockLast") = []
{
    auto options = WindowOptions {};
    options.onWillResize = [](int& width, int&) { width = std::min(width, 640); };
    options.sizeConstraint = [](const ResizeRequest& request)
    { return Point {request.size.x, request.size.y + 1000.f}; };
    options.aspectRatio = sixteenNine;

    // 800 clamps to 640, and the custom rule's height is overruled by the lock.
    check(
        same(options.effectiveSizeConstraint()({{800.f, 100.f}, ResizeAxis::Width}),
             {640.f, 360.f}));
};

auto tInitialSizeIsSnapped =
    test("SizeConstraint/initialSizeIsSnappedWidthFirst") = []
{
    auto options = WindowOptions {};
    options.width = 960;
    options.height = 400;
    options.sizeConstraint = AspectRatioLock {sixteenNine};

    check(same(options.effectiveInitialSize(), {960.f, 540.f}));
};

// The layout side of the same object: where the locked content goes. For a
// size the lock allowed, that is exactly the bounds less the border.
auto tLockedAreaIsTheBoundsLessTheBorder =
    test("SizeConstraint/lockedAreaIsTheBoundsLessTheBorder") = []
{
    auto lock = AspectRatioLock {sixteenNine, Insets {.top = 60.f, .right = 200.f}};

    check(
        same(lock.lockedArea({0.f, 0.f, 1160.f, 600.f}), {0.f, 60.f, 960.f, 540.f}));
};

// The reported bug: 1311 x 670 under a 56-point header beside a 220-point
// inspector leaves 1091 x 614, which the lock allowed (1091 / (16/9) is
// 613.69, rounded to the point). Re-deriving the ratio from it gave the
// canvas a height of 613.69 on a fractional y, which the scissor clip then
// cut. A size the lock allowed is used as it is.
auto tLockedAreaIsWholePointsForAnAllowedSize =
    test("SizeConstraint/lockedAreaIsWholePointsForAnAllowedSize") = []
{
    auto lock = AspectRatioLock {sixteenNine, Insets {.top = 56.f, .right = 220.f}};

    check(lock.allows({1311.f, 670.f}));
    check(same(lock.lockedArea({0.f, 0.f, 1311.f, 670.f}),
               {0.f, 56.f, 1091.f, 614.f}));

    // And one it arrived at from a height drag.
    auto fromHeight = lock({{0.f, 56.f + 601.f}, ResizeAxis::Height});
    check(lock.allows(fromHeight));
    check(same(lock.lockedArea({0.f, 0.f, fromHeight.x, fromHeight.y}),
               {0.f, 56.f, fromHeight.x - 220.f, 601.f}));
};

// For one it did not - fullscreen on a display of another shape, say - the
// content is letterboxed inside what the border leaves, centred, and still
// on whole points.
auto tLockedAreaLetterboxesAForeignSize =
    test("SizeConstraint/lockedAreaLetterboxesAForeignSize") = []
{
    auto lock = AspectRatioLock {sixteenNine, Insets {.top = 60.f}};

    // Too tall: bars above and below.
    check(same(lock.lockedArea({0.f, 0.f, 960.f, 1060.f}),
               {0.f, 60.f + 230.f, 960.f, 540.f}));

    // Too wide: bars left and right.
    check(same(lock.lockedArea({0.f, 0.f, 1360.f, 600.f}),
               {200.f, 60.f, 960.f, 540.f}));
};

auto tLockedAreaOfAnUnlockedRatioIsTheInset =
    test("SizeConstraint/lockedAreaOfAnUnlockedRatioIsTheInset") = []
{
    auto lock = AspectRatioLock {{0.f, 0.f}, Insets {.top = 60.f}};

    check(
        same(lock.lockedArea({0.f, 0.f, 800.f, 600.f}), {0.f, 60.f, 800.f, 540.f}));
};

// A bound is fitted, not driven: when the width-driven shape is too tall for
// it, the height drives instead. What a maximise, a zoom, a fullscreen and a
// too-small display all get.
auto tFitWithinPicksTheAxisThatFits =
    test("SizeConstraint/fitWithinPicksTheAxisThatFits") = []
{
    auto lock = SizeConstraint {AspectRatioLock {sixteenNine}};

    // Wide display: the height binds.
    check(same(fitWithin(lock, {2000.f, 540.f}), {960.f, 540.f}));

    // Tall display: the width binds.
    check(same(fitWithin(lock, {960.f, 2000.f}), {960.f, 540.f}));

    // An unconstrained window gets the whole of it.
    check(same(fitWithin(WindowOptions {}.sizeConstraint, {2000.f, 540.f}),
               {2000.f, 540.f}));
};

// A rule that fits neither way is clamped rather than let through: a bound is
// a bound.
auto tFitWithinClampsTheUnfittable =
    test("SizeConstraint/fitWithinClampsTheUnfittable") = []
{
    auto tooBig =
        SizeConstraint {[](const ResizeRequest&) { return Point {5000.f, 5000.f}; }};

    check(same(fitWithin(tooBig, {800.f, 600.f}), {800.f, 600.f}));
};

// AppKit and libdecor say what size, not which edge, so it is read off which
// dimension moved.
auto tAxisIsReadOffWhatMoved = test("SizeConstraint/axisIsReadOffWhatMoved") = []
{
    auto current = Point {800.f, 600.f};

    check(resizeAxisBetween(current, {900.f, 600.f}) == ResizeAxis::Width);
    check(resizeAxisBetween(current, {800.f, 700.f}) == ResizeAxis::Height);
    check(resizeAxisBetween(current, {900.f, 700.f}) == ResizeAxis::Both);
    check(resizeAxisBetween(current, current) == ResizeAxis::Both);
};

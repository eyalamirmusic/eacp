#include "Common.h"

// Rect had no tests at all, which is how its splitters came to disagree with
// the coordinate system every other part of the framework uses.
//
// The y-axis assertions below are the point of this file. Rect is y-down: y = 0
// is the top, matching isFlipped views, MouseEvent::pos, and the sprite, glyph
// and scissor paths. A splitter that returns the wrong half still compiles,
// still returns a rect of the right size, and still draws something — so
// nothing catches it except an assertion about which edge it came from.

using namespace nano;
using eacp::Graphics::Point;
using eacp::Graphics::Rect;

namespace
{
bool same(float a, float b)
{
    return std::abs(a - b) < 0.0001f;
}

bool same(const Rect& rect, const Rect& expected)
{
    return same(rect.x, expected.x) && same(rect.y, expected.y)
           && same(rect.w, expected.w) && same(rect.h, expected.h);
}
} // namespace

auto tEdges = test("Rect/edgesRunTopToBottom") = []
{
    const auto rect = Rect {10.f, 20.f, 100.f, 50.f};

    check(same(rect.left(), 10.f));
    check(same(rect.right(), 110.f));

    // The smaller y is the top. Reversing these two is the whole bug.
    check(same(rect.top(), 20.f));
    check(same(rect.bottom(), 70.f));
    check(rect.top() < rect.bottom());
};

auto tFromTopTakesTheTop = test("Rect/fromTopTakesTheTopEdge") = []
{
    const auto area = Rect {0.f, 0.f, 200.f, 100.f};

    check(same(area.fromTop(20.f), {0.f, 0.f, 200.f, 20.f}));
    check(same(area.fromBottom(20.f), {0.f, 80.f, 200.f, 20.f}));

    // A slice from the top starts at the rect's own top edge.
    check(same(area.fromTop(20.f).y, area.top()));
    check(same(area.fromBottom(20.f).bottom(), area.bottom()));
};

auto tFromTopMargin = test("Rect/fromTopMarginPushesInward") = []
{
    const auto area = Rect {0.f, 0.f, 200.f, 100.f};

    // The margin insets from the edge the slice was taken from, so both slices
    // move toward the middle rather than both moving down.
    check(same(area.fromTop(20.f, 5.f), {0.f, 5.f, 200.f, 20.f}));
    check(same(area.fromBottom(20.f, 5.f), {0.f, 75.f, 200.f, 20.f}));
};

auto tRemoveFromTop = test("Rect/removeFromTopTakesTheTopEdge") = []
{
    auto area = Rect {0.f, 0.f, 200.f, 100.f};

    const auto taken = area.removeFromTop(20.f);

    check(same(taken, {0.f, 0.f, 200.f, 20.f}));

    // What is left starts where the slice ended.
    check(same(area, {0.f, 20.f, 200.f, 80.f}));
};

auto tRemoveFromBottom = test("Rect/removeFromBottomTakesTheBottomEdge") = []
{
    auto area = Rect {0.f, 0.f, 200.f, 100.f};

    const auto taken = area.removeFromBottom(20.f);

    check(same(taken, {0.f, 80.f, 200.f, 20.f}));
    check(same(area, {0.f, 0.f, 200.f, 80.f}));
};

auto tChromeLayout = test("Rect/aWindowSplitsIntoChrome") = []
{
    // The layout every app built on this does: bars off the edges, editor in
    // what is left. Written out because it is the case that was wrong.
    auto area = Rect {0.f, 0.f, 1000.f, 600.f};

    const auto tabBar = area.removeFromTop(35.f);
    const auto statusBar = area.removeFromBottom(22.f);
    const auto sidebar = area.removeFromLeft(240.f);

    check(same(tabBar, {0.f, 0.f, 1000.f, 35.f}));
    check(same(statusBar, {0.f, 578.f, 1000.f, 22.f}));
    check(same(sidebar, {0.f, 35.f, 240.f, 543.f}));
    check(same(area, {240.f, 35.f, 760.f, 543.f}));

    // The tab bar is above the status bar, and nothing overlaps.
    check(tabBar.bottom() <= area.top());
    check(area.bottom() <= statusBar.top());
};

auto tSplittersLeaveNoGaps = test("Rect/splittersPartitionExactly") = []
{
    auto area = Rect {5.f, 7.f, 300.f, 200.f};
    const auto whole = area;

    const auto top = area.removeFromTop(30.f);
    const auto bottom = area.removeFromBottom(40.f);

    // The three pieces tile the original with no gap and no overlap.
    check(same(top.top(), whole.top()));
    check(same(top.bottom(), area.top()));
    check(same(area.bottom(), bottom.top()));
    check(same(bottom.bottom(), whole.bottom()));
    check(same(top.h + area.h + bottom.h, whole.h));
};

auto tHorizontalSplittersUnchanged =
    test("Rect/horizontalSplittersAreUnaffected") = []
{
    auto area = Rect {0.f, 0.f, 200.f, 100.f};

    check(same(area.fromLeft(20.f), {0.f, 0.f, 20.f, 100.f}));
    check(same(area.fromRight(20.f), {180.f, 0.f, 20.f, 100.f}));

    const auto taken = area.removeFromLeft(20.f);

    check(same(taken, {0.f, 0.f, 20.f, 100.f}));
    check(same(area, {20.f, 0.f, 180.f, 100.f}));
};

auto tContains = test("Rect/containsIsHalfOpen") = []
{
    const auto rect = Rect {10.f, 20.f, 100.f, 50.f};

    check(rect.contains({10.f, 20.f}));
    check(rect.contains({109.f, 69.f}));

    // The far edges belong to the next rect along, so tiled rects do not both
    // claim a point on their shared border.
    check(!rect.contains({110.f, 40.f}));
    check(!rect.contains({40.f, 70.f}));
    check(!rect.contains({9.f, 40.f}));
};

auto tInsetIsSymmetric = test("Rect/insetPullsInFromEveryEdge") = []
{
    const auto rect = Rect {0.f, 0.f, 100.f, 60.f};

    check(same(rect.inset(10.f), {10.f, 10.f, 80.f, 40.f}));
    check(same(rect.inset(10.f, 5.f), {10.f, 5.f, 80.f, 50.f}));
};

auto tIsEmpty = test("Rect/isEmptyCoversCollapsedAndInsideOut") = []
{
    check(Rect {}.isEmpty());
    check(Rect {10.f, 10.f, 0.f, 50.f}.isEmpty());
    check(Rect {10.f, 10.f, 50.f, 0.f}.isEmpty());
    check(!Rect {10.f, 10.f, 1.f, 1.f}.isEmpty());

    // An inset bigger than the rect leaves negative extents, which must read as
    // empty rather than as a rect that draws inside out.
    check(Rect {0.f, 0.f, 10.f, 10.f}.inset(20.f).isEmpty());
};

auto tIntersection = test("Rect/intersectionIsTheOverlap") = []
{
    const auto a = Rect {0.f, 0.f, 100.f, 100.f};
    const auto b = Rect {50.f, 25.f, 100.f, 100.f};

    check(a.intersects(b));
    check(b.intersects(a));
    check(same(a.intersection(b), {50.f, 25.f, 50.f, 75.f}));

    // Order does not matter.
    check(same(b.intersection(a), a.intersection(b)));

    // A rect wholly inside another is the intersection.
    const auto inner = Rect {10.f, 10.f, 20.f, 20.f};
    check(same(a.intersection(inner), inner));
};

auto tIntersectionWhenDisjoint = test("Rect/disjointRectsIntersectToEmpty") = []
{
    const auto a = Rect {0.f, 0.f, 50.f, 50.f};
    const auto b = Rect {100.f, 100.f, 50.f, 50.f};

    check(!a.intersects(b));
    check(a.intersection(b).isEmpty());

    // No negative extents, so nesting clips can keep intersecting a rect that
    // has already gone empty without it growing back.
    const auto empty = a.intersection(b);
    check(empty.w >= 0.f && empty.h >= 0.f);
    check(empty.intersection(a).isEmpty());
};

auto tTouchingRectsDoNotIntersect = test("Rect/rectsSharingAnEdgeDoNotOverlap") = []
{
    const auto a = Rect {0.f, 0.f, 50.f, 50.f};
    const auto b = Rect {50.f, 0.f, 50.f, 50.f};

    // Consistent with contains() being half-open: tiled rects share a border
    // and claim no pixel twice.
    check(!a.intersects(b));
    check(a.intersection(b).isEmpty());
};

auto tCenter = test("Rect/centerIsTheMiddle") = []
{
    const auto middle = Rect {10.f, 20.f, 100.f, 50.f}.center();

    check(same(middle.x, 60.f));
    check(same(middle.y, 45.f));
};

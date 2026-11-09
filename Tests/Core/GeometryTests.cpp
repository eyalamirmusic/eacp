#include "Common.h"

using namespace nano;
using namespace eacp::Geometry;

namespace GeometryTests
{
bool isSameRectangle(const Rectangle<int>& a, const Rectangle<int>& b)
{
    return a.x == b.x && a.y == b.y && a.w == b.w && a.h == b.h;
}

auto tRoundsFractionalEdgesOutward =
    test("Geometry/roundsFractionalEdgesOutward") = []
{
    auto pixels = Rectangle<float> {1.25f, 2.75f, 3.5f, 4.f}.roundedOutwardTo<int>();
    check(isSameRectangle(pixels, {1, 2, 4, 5}));
};

auto tKeepsWholeEdgesWhenRoundingOutward =
    test("Geometry/keepsWholeEdgesWhenRoundingOutward") = []
{
    auto pixels = Rectangle<float> {2.f, 3.f, 10.f, 20.f}.roundedOutwardTo<int>();
    check(isSameRectangle(pixels, {2, 3, 10, 20}));
};

auto tRoundsNegativeEdgesAwayFromTheRectangle =
    test("Geometry/roundsNegativeEdgesAwayFromTheRectangle") = []
{
    auto pixels =
        Rectangle<float> {-1.5f, -0.25f, 1.f, 0.5f}.roundedOutwardTo<int>();
    check(isSameRectangle(pixels, {-2, -1, 2, 2}));
};

auto tConvertsByPlainCast = test("Geometry/convertsByPlainCast") = []
{
    auto converted = Rectangle<float> {1.9f, -1.9f, 2.5f, 3.5f}.to<int>();
    check(isSameRectangle(converted, {1, -1, 2, 3}));

    auto widened = Rectangle<int> {1, 2, 3, 4}.to<float>();
    check(widened.x == 1.f && widened.y == 2.f && widened.w == 3.f
          && widened.h == 4.f);

    auto point = Point<float> {1.5f, -2.5f}.to<int>();
    check(point.x == 1 && point.y == -2);
};

auto tContainsRectanglesInsideItsEdges =
    test("Geometry/containsRectanglesInsideItsEdges") = []
{
    auto outer = Rectangle<int> {0, 0, 10, 10};
    check(outer.contains(Rectangle<int> {0, 0, 10, 10}));
    check(outer.contains(Rectangle<int> {2, 2, 3, 3}));
    check(!outer.contains(Rectangle<int> {8, 8, 3, 3}));
    check(!outer.contains(Rectangle<int> {-1, 0, 5, 5}));
    check(outer.contains(Point<int> {0, 0}));
    check(!outer.contains(Point<int> {10, 5}));
};

auto tIntersectsOnlyWhenAreasOverlap =
    test("Geometry/intersectsOnlyWhenAreasOverlap") = []
{
    auto a = Rectangle<int> {0, 0, 10, 10};
    check(a.intersects(Rectangle<int> {5, 5, 10, 10}));
    check(!a.intersects(Rectangle<int> {10, 0, 5, 5}));
    check(!a.intersects(Rectangle<int> {2, 2, 0, 5}));
    check(isSameRectangle(a.intersection({5, 5, 10, 10}), {5, 5, 5, 5}));
    check(a.intersection({20, 20, 5, 5}).isEmpty());

    auto f = Rectangle<float> {0.f, 0.f, 1.f, 1.f};
    check(f.intersects(Rectangle<float> {0.5f, 0.5f, 1.f, 1.f}));
    check(!f.intersects(Rectangle<float> {1.f, 0.f, 1.f, 1.f}));
};

auto tIsEmptyWithoutPositiveArea = test("Geometry/isEmptyWithoutPositiveArea") = []
{
    check(Rectangle<int> {}.isEmpty());
    check(Rectangle<int> {0, 0, 5, 0}.isEmpty());
    check(Rectangle<float> {0.f, 0.f, -1.f, 5.f}.isEmpty());
    check(!Rectangle<float> {0.f, 0.f, 0.5f, 0.5f}.isEmpty());
};

auto tSplitsIntegerRectangles = test("Geometry/splitsIntegerRectangles") = []
{
    auto area = Rectangle<int> {0, 0, 10, 20};
    auto header = area.removeFromTop(5);
    check(isSameRectangle(header, {0, 0, 10, 5}));
    check(isSameRectangle(area, {0, 5, 10, 15}));
    check(isSameRectangle(area.inset(Insets<int> {.left = 1, .right = 2}),
                          {1, 5, 7, 15}));
};
} // namespace GeometryTests

#include "Common.h"

using namespace nano;
using namespace eacp::Geometry;

namespace GeometryTests
{
auto tRoundsFractionalEdgesOutward =
    test("Geometry/roundsFractionalEdgesOutward") = []
{
    auto pixels = Rectangle<float> {1.25f, 2.75f, 3.5f, 4.f}.roundedOutwardTo<int>();
    check(pixels == Rectangle<int> {1, 2, 4, 5});
};

auto tKeepsWholeEdgesWhenRoundingOutward =
    test("Geometry/keepsWholeEdgesWhenRoundingOutward") = []
{
    auto pixels = Rectangle<float> {2.f, 3.f, 10.f, 20.f}.roundedOutwardTo<int>();
    check(pixels == Rectangle<int> {2, 3, 10, 20});
};

auto tRoundsNegativeEdgesAwayFromTheRectangle =
    test("Geometry/roundsNegativeEdgesAwayFromTheRectangle") = []
{
    auto pixels =
        Rectangle<float> {-1.5f, -0.25f, 1.f, 0.5f}.roundedOutwardTo<int>();
    check(pixels == Rectangle<int> {-2, -1, 2, 2});
};

auto tConvertsByPlainCast = test("Geometry/convertsByPlainCast") = []
{
    auto converted = Rectangle<float> {1.9f, -1.9f, 2.5f, 3.5f}.to<int>();
    check(converted == Rectangle<int> {1, -1, 2, 3});

    auto widened = Rectangle<int> {1, 2, 3, 4}.to<float>();
    check(widened == Rectangle<float> {1.f, 2.f, 3.f, 4.f});

    auto point = Point<float> {1.5f, -2.5f}.to<int>();
    check(point == Point<int> {1, -2});
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
    check(a.intersection({5, 5, 10, 10}) == Rectangle<int> {5, 5, 5, 5});
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
    check(header == Rectangle<int> {0, 0, 10, 5});
    check(area == Rectangle<int> {0, 5, 10, 15});
    check(area.inset(Insets<int> {.left = 1, .right = 2})
          == Rectangle<int> {1, 5, 7, 15});
};
static_assert(Rectangle<int> {0, 0, 10, 10}.contains(Rectangle<int> {2, 2, 3, 3}));
static_assert(Rectangle<int> {0, 0, 10, 10}.intersects({5, 5, 10, 10}));
static_assert(Rectangle<int> {0, 0, 10, 10}.intersection({20, 20, 5, 5}).isEmpty());
static_assert(Rectangle<float> {1.5f, 0.f, 1.f, 1.f}.to<int>().x == 1);
static_assert((Point<int> {1, 2} + Point<int> {3, 4}).x == 4);
static_assert(Insets<int> {.top = 1, .bottom = 2}.size().y == 3);
static_assert(Rectangle<int> {1, 2, 3, 4} == Rectangle<int> {1, 2, 3, 4});
static_assert(Rectangle<int> {1, 2, 3, 4} != Rectangle<int> {1, 2, 3, 5});
} // namespace GeometryTests

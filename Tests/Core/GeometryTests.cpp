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
auto tAddsAndSubtractsPoints = test("Geometry/addsAndSubtractsPoints") = []
{
    check(Point<int> {1, 2} + Point<int> {3, 4} == Point<int> {4, 6});
    check(Point<double> {1.5, 2.} - Point<double> {0.5, 3.}
          == Point<double> {1., -1.});
    check(Insets<int> {.top = 1, .bottom = 2}.size() == Point<int> {0, 3});
};

auto tRoundsIntegerLengthsToTheNearest =
    test("Geometry/roundsIntegerLengthsToTheNearest") = []
{
    check(Point<int> {3, 4}.length() == 5);
    check(Point<int> {1, 1}.length() == 1);
    check(Point<int> {2, 2}.length() == 3);
    check(Point<int> {0, 0}.distanceTo({-6, 8}) == 10);
    check(Point<float> {3.f, 4.f}.length() == 5.f);
};

auto tNormalizesIntegerPointsToTheNearestUnitStep =
    test("Geometry/normalizesIntegerPointsToTheNearestUnitStep") = []
{
    check(Point<int> {10, 0}.normalized() == Point<int> {1, 0});
    check(Point<int> {-3, -4}.normalized() == Point<int> {-1, -1});
    check(Point<int> {0, 0}.normalized() == Point<int> {});
    check(Point<double> {0., -2.}.normalized() == Point<double> {0., -1.});
};

auto tMapsIntoIntegerRectanglesWithoutDividingByZero =
    test("Geometry/mapsIntoIntegerRectanglesWithoutDividingByZero") = []
{
    auto rect = Rectangle<int> {0, 0, 0, 4};
    check(rect.getRelativePoint({3, 4}) == Point<int> {0, 1});
    check(Rectangle<int> {0, 0, 5, 7}.center() == Point<int> {2, 3});
    check(Rectangle<double> {0., 0., 4., 2.}.getRelativePoint({1., 1.})
          == Point<double> {0.25, 0.5});
};

auto tComparesEveryMember = test("Geometry/comparesEveryMember") = []
{
    check(Rectangle<int> {1, 2, 3, 4} == Rectangle<int> {1, 2, 3, 4});
    check(Rectangle<int> {1, 2, 3, 4} != Rectangle<int> {1, 2, 3, 5});
    check(Insets<double> {.top = 1.} != Insets<double> {.left = 1.});
};
} // namespace GeometryTests

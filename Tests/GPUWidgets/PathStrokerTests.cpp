#include "CoverageProbe.h"

#include <NanoTest/NanoTest.h>

#include <cmath>

using namespace nano;
using namespace eacp;
using namespace eacp::GPUWidgets;
using eacp::GPUWidgets::probe::rasterize;

namespace
{
using Graphics::Point;

constexpr auto pi = 3.14159265358979323846f;

// Pixels within this of the ideal edge are in the antialiased band and are left
// unjudged; the checks are about which region got filled, not how it is shaded.
constexpr auto margin = 1.5f;

float signedArea(const Vector<Point>& contour)
{
    auto sum = 0.f;
    auto count = contour.size();

    for (auto i = 0; i < count; ++i)
    {
        const auto& a = contour[i];
        const auto& b = contour[(i + 1) % count];
        sum += a.x * b.y - b.x * a.y;
    }

    return sum * 0.5f;
}

float distanceToSegment(const Point& p, const Point& a, const Point& b)
{
    auto dx = b.x - a.x;
    auto dy = b.y - a.y;
    auto lengthSquared = dx * dx + dy * dy;

    auto t = lengthSquared > 0.f
                 ? std::clamp(((p.x - a.x) * dx + (p.y - a.y) * dy) / lengthSquared,
                              0.f,
                              1.f)
                 : 0.f;

    auto nearestX = a.x + dx * t;
    auto nearestY = a.y + dy * t;

    return std::sqrt((p.x - nearestX) * (p.x - nearestX)
                     + (p.y - nearestY) * (p.y - nearestY));
}

float distanceToPath(const Point& p, const Path& path)
{
    auto best = std::numeric_limits<float>::max();

    for (const auto& sub: path.getSubPaths())
    {
        const auto& points = sub.points;

        if (points.size() == 1)
            best = std::min(best, distanceToSegment(p, points[0], points[0]));

        auto segments = sub.closed ? points.size() : points.size() - 1;

        for (auto i = 0; i < segments; ++i)
            best = std::min(
                best,
                distanceToSegment(p, points[i], points[(i + 1) % points.size()]));
    }

    return best;
}

Path openPolyline()
{
    auto path = Path {};
    path.moveTo({20.f, 30.f});
    path.lineTo({70.f, 30.f});
    path.lineTo({70.f, 80.f});
    path.lineTo({120.f, 80.f});
    return path;
}

Path closedSquare()
{
    auto path = Path {};
    path.addRect({25.f, 25.f, 90.f, 70.f});
    return path;
}

Path circle()
{
    auto path = Path {};
    path.addEllipse({20.f, 20.f, 140.f, 140.f});
    return path;
}

Path hairpin()
{
    auto path = Path {};
    path.moveTo({30.f, 30.f});
    path.lineTo({140.f, 40.f});
    path.lineTo({30.f, 50.f});
    return path;
}

Path zigzag()
{
    auto path = Path {};
    path.moveTo({20.f, 60.f});

    for (auto i = 1; i <= 6; ++i)
        path.lineTo({20.f + (float) i * 22.f, i % 2 == 0 ? 60.f : 20.f});

    return path;
}

Vector<Path> everyShape()
{
    auto shapes = Vector<Path> {};
    shapes.add(openPolyline());
    shapes.add(closedSquare());
    shapes.add(circle());
    shapes.add(hairpin());
    shapes.add(zigzag());
    return shapes;
}

Vector<StrokeStyle> everyStyle(float width)
{
    auto styles = Vector<StrokeStyle> {};

    for (auto join: {LineJoin::Miter, LineJoin::Round, LineJoin::Bevel})
        for (auto cap: {LineCap::Butt, LineCap::Round, LineCap::Square})
            styles.add(StrokeStyle {width, cap, join, 4.f});

    return styles;
}

struct Verdict
{
    bool ran = false;
    int missing = 0; // well inside the stroke, and not filled
    int extra = 0; // well outside it, and filled anyway
    Point worstMissing;
};

Verdict checkAgainstDistance(const Path& source, float width, float scale)
{
    auto verdict = Verdict {};
    auto style = StrokeStyle {width, LineCap::Round, LineJoin::Round, 4.f};
    auto outline = strokeToFill(source, style);

    auto rasterizer = PathRasterizer {};
    auto coverage = rasterize(rasterizer, outline, scale);

    if (coverage.empty())
        return verdict;

    verdict.ran = true;

    auto covered = rasterizer.getCoveredBounds();
    auto pixelWidth = rasterizer.getCoverageWidth();
    auto half = width * 0.5f;

    for (auto y = 0; y < rasterizer.getCoverageHeight(); ++y)
    {
        for (auto x = 0; x < pixelWidth; ++x)
        {
            auto at = Point {covered.x + ((float) x + 0.5f) / scale,
                             covered.y + ((float) y + 0.5f) / scale};

            auto distance = distanceToPath(at, source);
            auto value = coverage[y * pixelWidth + x];

            if (distance < half - margin / scale && value < 0.99f)
            {
                if (verdict.missing == 0)
                    verdict.worstMissing = at;

                ++verdict.missing;
            }

            if (distance > half + margin / scale && value > 0.01f)
                ++verdict.extra;
        }
    }

    return verdict;
}
} // namespace

// Contours that disagree subtract where they overlap, so a join wound backwards
// removes the corner it was there to fill.
auto tWinding = test("PathStroker/everyContourWindsTheSameWay") = []
{
    for (const auto& shape: everyShape())
    {
        for (const auto& style: everyStyle(9.f))
        {
            auto outline = strokeToFill(shape, style);
            auto positive = 0;
            auto negative = 0;

            for (const auto& sub: outline.getSubPaths())
            {
                auto area = signedArea(sub.points);

                if (area > 0.f)
                    ++positive;
                else if (area < 0.f)
                    ++negative;
            }

            check(positive == 0 || negative == 0,
                  std::to_string(positive) + " contours one way, "
                      + std::to_string(negative) + " the other");
        }
    }
};

// A fill treats an unclosed contour as closed anyway, so an unclosed piece
// would only show up somewhere else.
auto tClosed = test("PathStroker/everyContourIsClosed") = []
{
    for (const auto& shape: everyShape())
    {
        for (const auto& style: everyStyle(6.f))
        {
            auto outline = strokeToFill(shape, style);

            for (const auto& sub: outline.getSubPaths())
            {
                check(sub.closed);
                check(sub.points.size() >= 3);
            }
        }
    }
};

// A round-joined, round-capped stroke is exactly the points within half the
// width of the source polyline, so the distance predicate is the reference.
auto tCoversTheRibbon = test("PathStroker/roundStrokeIsTheDistanceField") = []
{
    for (const auto& shape: everyShape())
    {
        for (auto width: {5.f, 13.f})
        {
            auto verdict = checkAgainstDistance(shape, width, 2.f);

            if (!verdict.ran)
                return;

            check(verdict.missing == 0,
                  std::to_string(verdict.missing) + " pixels inside the stroke left "
                      + "unfilled, first at ("
                      + std::to_string(verdict.worstMissing.x) + ", "
                      + std::to_string(verdict.worstMissing.y) + ")");

            check(verdict.extra == 0,
                  std::to_string(verdict.extra)
                      + " pixels filled outside the stroke");
        }
    }
};

auto tCapsReach = test("PathStroker/capsReachAsFarAsTheyShould") = []
{
    auto line = Path {};
    line.moveTo({40.f, 50.f});
    line.lineTo({120.f, 50.f});

    auto width = 10.f;
    auto reachOf = [&](LineCap cap)
    {
        auto outline = strokeToFill(line, StrokeStyle {width, cap});
        return outline.getBounds();
    };

    auto butt = reachOf(LineCap::Butt);
    auto square = reachOf(LineCap::Square);
    auto round = reachOf(LineCap::Round);

    check(std::abs(butt.x - 40.f) < 0.01f);
    check(std::abs(butt.w - 80.f) < 0.01f);

    check(std::abs(square.x - 35.f) < 0.01f);
    check(std::abs(square.w - 90.f) < 0.01f);

    // A round cap is a polygon, so it only reaches to within the flattening
    // tolerance rather than to 0.01 like the other two.
    check(std::abs(round.x - 35.f) < line.getFlatness());
};

auto tClosedHasNoCaps = test("PathStroker/aClosedPathIgnoresTheCap") = []
{
    auto style = StrokeStyle {8.f, LineCap::Butt, LineJoin::Miter, 4.f};
    auto butt = strokeToFill(closedSquare(), style).getBounds();

    style.cap = LineCap::Square;
    auto square = strokeToFill(closedSquare(), style).getBounds();

    check(std::abs(butt.x - square.x) < 0.01f);
    check(std::abs(butt.w - square.w) < 0.01f);
    check(std::abs(butt.h - square.h) < 0.01f);
};

auto tMiterLimit = test("PathStroker/theMiterLimitCapsTheCorner") = []
{
    auto width = 10.f;
    auto reachOf = [&](float limit)
    {
        auto style = StrokeStyle {width, LineCap::Butt, LineJoin::Miter, limit};
        return strokeToFill(hairpin(), style).getBounds();
    };

    auto generous = reachOf(40.f);
    auto strict = reachOf(1.f);

    // The hairpin turns at x = 140.
    check(generous.x + generous.w > 175.f);
    check(strict.x + strict.w < 150.f);

    auto byDefault = strokeToFill(hairpin(), StrokeStyle {width}).getBounds();
    check(byDefault.x + byDefault.w < 160.f);
};

// A smooth corner is beveled whatever join was asked for, since a disc at each
// flattened vertex would be most of the stroke's segments.
auto tSmoothCurveBevels = test("PathStroker/aFlattenedCurveGetsCheapJoins") = []
{
    auto style = StrokeStyle {6.f, LineCap::Butt, LineJoin::Round, 4.f};
    auto outline = strokeToFill(circle(), style);

    auto sourceSegments = circle().getSubPaths()[0].points.size();

    // A quad and a three-point join for every segment.
    check(outline.getSubPaths().size() == sourceSegments * 2,
          std::to_string(outline.getSubPaths().size()) + " contours for "
              + std::to_string(sourceSegments) + " segments");

    auto triangles = 0;

    for (const auto& sub: outline.getSubPaths())
        if (sub.points.size() == 3)
            ++triangles;

    check(triangles == sourceSegments,
          std::to_string(triangles) + " beveled corners of "
              + std::to_string(sourceSegments));

    auto square = strokeToFill(closedSquare(), style);
    auto discs = 0;

    for (const auto& sub: square.getSubPaths())
        if (sub.points.size() > 4)
            ++discs;

    check(discs == 4, std::to_string(discs) + " round joins on a square");
};

auto tDot = test("PathStroker/aZeroLengthPathIsADot") = []
{
    auto dot = Path {};
    dot.moveTo({50.f, 50.f});
    dot.lineTo({50.f, 50.f});

    check(strokeToFill(dot, StrokeStyle {8.f, LineCap::Butt}).isEmpty());
    check(!strokeToFill(dot, StrokeStyle {8.f, LineCap::Round}).isEmpty());
    check(!strokeToFill(dot, StrokeStyle {8.f, LineCap::Square}).isEmpty());

    auto bounds = strokeToFill(dot, StrokeStyle {8.f, LineCap::Square}).getBounds();
    check(std::abs(bounds.w - 8.f) < 0.01f);
    check(std::abs(bounds.h - 8.f) < 0.01f);
};

auto tDegenerate = test("PathStroker/nothingToStrokeIsNoPath") = []
{
    check(strokeToFill(Path {}, StrokeStyle {4.f}).isEmpty());
    check(strokeToFill(closedSquare(), StrokeStyle {0.f}).isEmpty());
    check(strokeToFill(closedSquare(), StrokeStyle {-2.f}).isEmpty());
};

namespace
{
Path straightLine(float length)
{
    auto path = Path {};
    path.moveTo({0.f, 0.f});
    path.lineTo({length, 0.f});

    return path;
}

float lengthOf(const Path& path)
{
    auto total = 0.f;

    for (const auto& sub: path.getSubPaths())
        for (auto i = 1; i < sub.points.size(); ++i)
            total += std::hypot(sub.points[i].x - sub.points[i - 1].x,
                                sub.points[i].y - sub.points[i - 1].y);

    return total;
}
} // namespace

auto tDashCutsIntoOnLengths = test("PathStroker/aDashPatternCutsThePolylineUp") = []
{
    // Ten units of "2 on, 2 off" is on at 0-2, 4-6 and 8-10.
    auto dashes = dashPath(straightLine(10.f), {{2.f, 2.f}, 0.f});

    check(dashes.getSubPaths().size() == 3,
          std::to_string(dashes.getSubPaths().size()) + " dashes");
    check(std::abs(lengthOf(dashes) - 6.f) < 0.01f, "three on-lengths of two");

    check(std::abs(dashes.getSubPaths()[1].points[0].x - 4.f) < 0.01f,
          "the second starts where the first off-length ends");
};

// An odd list is written out twice, so "3" is three on and three off.
auto tOddPatternIsDoubled = test("PathStroker/anOddDashListIsWrittenOutTwice") = []
{
    auto dashes = dashPath(straightLine(12.f), {{3.f}, 0.f});

    check(dashes.getSubPaths().size() == 2);
    check(std::abs(lengthOf(dashes) - 6.f) < 0.01f);
};

auto tDashOffset = test("PathStroker/theOffsetStartsPartWayIntoThePattern") = []
{
    // Two units into "2 on 2 off" is an off-length, so the line begins in a gap.
    auto dashes = dashPath(straightLine(10.f), {{2.f, 2.f}, 2.f});

    check(std::abs(dashes.getSubPaths()[0].points[0].x - 2.f) < 0.01f);
    check(std::abs(lengthOf(dashes) - 4.f) < 0.01f, "two on-lengths, not three");

    check(!dashPath(straightLine(10.f), {{2.f, 2.f}, -3.f}).isEmpty());
};

auto tClosedPathDashesRound =
    test("PathStroker/aClosedSubPathDashesThroughItsJoin") = []
{
    auto square = Path {};
    square.addRect({0.f, 0.f, 10.f, 10.f});

    auto dashes = dashPath(square, {{20.f, 20.f}, 0.f});

    // The 40-unit perimeter under "20 on 20 off" is one dash of twenty, which is
    // only twenty if the closing edge was walked.
    check(std::abs(lengthOf(dashes) - 20.f) < 0.01f,
          std::to_string(lengthOf(dashes)) + " of a 40-unit perimeter");

    check(dashes.getSubPaths().size() == 1);
    check(!dashes.getSubPaths()[0].closed,
          "a dash is open however closed the source");
};

auto tNothingToDashBy =
    test("PathStroker/aPatternThatCannotCutHandsThePathBack") = []
{
    auto line = straightLine(10.f);

    check(std::abs(lengthOf(dashPath(line, {})) - 10.f) < 0.01f, "no lengths");
    check(std::abs(lengthOf(dashPath(line, {{0.f, 0.f}, 0.f})) - 10.f) < 0.01f,
          "lengths adding to nothing");

    // A negative entry invalidates the whole list rather than being clamped.
    check(std::abs(lengthOf(dashPath(line, {{4.f, -2.f}, 0.f})) - 10.f) < 0.01f);
};

auto tDashesStroke = test("PathStroker/dashesAreStrokedLikeAnyOtherPath") = []
{
    auto dashes = dashPath(straightLine(10.f), {{2.f, 2.f}, 0.f});
    auto region = strokeToFill(dashes, StrokeStyle {2.f, LineCap::Butt});

    check(!region.isEmpty());

    auto bounds = region.getBounds();
    check(std::abs(bounds.h - 2.f) < 0.01f, "the stroke's own width, across");
    check(std::abs(bounds.w - 10.f) < 0.01f, "and the line's length, along");
};

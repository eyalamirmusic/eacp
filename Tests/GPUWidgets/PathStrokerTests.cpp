#include "CoverageProbe.h"

#include <NanoTest/NanoTest.h>

#include <cmath>

// strokeToFill turns a stroke into a fill by emitting the pieces of the stroke
// as overlapping closed contours and letting the non-zero rule union them. Two
// things have to hold for that to be a stroke rather than a mess, and both are
// silent when they break.
//
// Every contour must wind the same way. Two that disagree subtract where they
// overlap, so a join wound backwards does not merely look wrong - it removes the
// corner it was there to fill. That is checked directly, on the geometry.
//
// And the union has to be the right region. With round joins and round caps the
// stroke is exactly the set of points within half the width of the source
// polyline, which is a predicate a test can evaluate per pixel without knowing
// anything about how the stroker got there. So the coverage is checked against
// that distance instead of against a reference implementation - a well-inside
// pixel must be full, a well-outside pixel must be empty, and the band between
// them is left alone because that is where antialiasing lives.

using namespace nano;
using namespace eacp;
using namespace eacp::GPUWidgets;
using eacp::GPUWidgets::probe::rasterize;

namespace
{
using Graphics::Point;

// How far from the ideal edge a pixel has to be before its coverage is called.
// One pixel each way covers the antialiased band; the checks are about which
// region got filled, not about how its edge is shaded.
constexpr auto margin = 1.5f;

// ------------------------------------------------------------------ geometry

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

// -------------------------------------------------------------------- shapes

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

// A hairpin: the sharpest corner a miter can be asked for, and the one the
// miter limit exists to refuse.
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

// --------------------------------------------------------------------- checks

// Coverage against the distance predicate, which is exact for a round-joined,
// round-capped stroke.
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
            // The centre of the pixel, back in the path's own units.
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

// The invariant the whole approach rests on. Every contour of every stroke of
// every shape has to agree, or the pieces subtract instead of uniting.
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

// Every piece is a closed contour, and a fill treats an unclosed one as closed
// anyway - so an unclosed piece would still fill, and the mistake would only
// show up somewhere else. Pinned here instead.
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

// The region, for the join and cap the distance predicate describes exactly.
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

// A butt cap stops on the last point. A square one goes half a width past it,
// and a round one the same but curved - so of the three only butt keeps the
// stroke inside the path's own bounds.
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

    // A round cap is a polygon, so it reaches half a width past the end only to
    // within the flattening tolerance - which is the same promise every other
    // curve in a Path makes.
    check(std::abs(round.x - 35.f) < line.getFlatness());
};

// A closed sub-path has no ends, so no cap style may change it.
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

// The miter limit is the whole reason miter is safe to default to: without it a
// hairpin's corner runs away to many times the stroke width.
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

    // The hairpin turns at x = 140; a generous limit lets the corner run well
    // past it and a strict one bevels it back to the offset points.
    check(generous.right() > 175.f);
    check(strict.right() < 150.f);

    // And the default has to be one of the safe ones.
    auto byDefault = strokeToFill(hairpin(), StrokeStyle {width}).getBounds();
    check(byDefault.right() < 160.f);
};

// Flattening a curve makes hundreds of corners that turn by almost nothing.
// A disc at each would be most of the stroke's segments for a corner a triangle
// covers to within the flattening error - so a smooth corner is beveled whatever
// join was asked for.
//
// What it must not do is leave the corner out. Two quads meeting at a vertex
// gap on the outside of the turn, and that gap runs from the outer edge down to
// the vertex: as deep as the stroke is wide, however gently the path turns.
auto tSmoothCurveBevels = test("PathStroker/aFlattenedCurveGetsCheapJoins") = []
{
    auto style = StrokeStyle {6.f, LineCap::Butt, LineJoin::Round, 4.f};
    auto outline = strokeToFill(circle(), style);

    auto sourceSegments = circle().getSubPaths()[0].points.size();

    // A quad and a join for every segment, and the join is the three-point one.
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

    // A real corner still gets the join it asked for, which is a whole disc.
    auto square = strokeToFill(closedSquare(), style);
    auto discs = 0;

    for (const auto& sub: square.getSubPaths())
        if (sub.points.size() > 4)
            ++discs;

    check(discs == 4, std::to_string(discs) + " round joins on a square");
};

// A zero-length sub-path is the "dot" a moveTo and a lineTo to the same place
// mean. Round and square caps draw one; a butt cap has nothing to extend.
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

// ----------------------------------------------------------------- dashing
//
// Dashing is a separate operation applied *before* stroking, and the order is
// not a preference: a dash cuts the centre line, and by the time strokeToFill
// has run the centre line has been replaced by the region around it. Cutting
// afterwards would have nothing with a length left to cut.
//
// So what dashPath produces is readable geometry - open sub-paths along the
// original polyline - and that is what these check, rather than coverage.

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
            total += sub.points[i].distanceTo(sub.points[i - 1]);

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

// The one thing about dashing that reliably surprises: an odd list is written
// out twice, so the entries alternate on and off rather than repeating. "3"
// alone is three on and three off, not three on for ever.
auto tOddPatternIsDoubled = test("PathStroker/anOddDashListIsWrittenOutTwice") = []
{
    auto dashes = dashPath(straightLine(12.f), {{3.f}, 0.f});

    check(dashes.getSubPaths().size() == 2);
    check(std::abs(lengthOf(dashes) - 6.f) < 0.01f);
};

auto tDashOffset = test("PathStroker/theOffsetStartsPartWayIntoThePattern") = []
{
    // Two units into "2 on 2 off" is the start of an off-length, so the line
    // begins in a gap.
    auto dashes = dashPath(straightLine(10.f), {{2.f, 2.f}, 2.f});

    check(std::abs(dashes.getSubPaths()[0].points[0].x - 2.f) < 0.01f);
    check(std::abs(lengthOf(dashes) - 4.f) < 0.01f, "two on-lengths, not three");

    // A negative offset is the same walk from the other end of the cycle, and
    // has to land somewhere in it rather than off it.
    check(!dashPath(straightLine(10.f), {{2.f, 2.f}, -3.f}).isEmpty());
};

// The closing edge is a segment like any other, which is what makes a dashed
// outline go round the corner it started at instead of stopping short of it.
auto tClosedPathDashesRound =
    test("PathStroker/aClosedSubPathDashesThroughItsJoin") = []
{
    auto square = Path {};
    square.addRect({0.f, 0.f, 10.f, 10.f});

    auto dashes = dashPath(square, {{20.f, 20.f}, 0.f});

    // Forty units of perimeter under "20 on 20 off" is one dash of twenty, and
    // it can only be twenty if the closing edge was walked.
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

    // A negative entry invalidates the whole list rather than being clamped
    // away, because a document that wrote one did not mean any of it.
    check(std::abs(lengthOf(dashPath(line, {{4.f, -2.f}, 0.f})) - 10.f) < 0.01f);
};

// The whole reason it is a path operation and not a stroke option: what comes
// out is something strokeToFill can take, and each dash is a piece with two open
// ends and therefore two caps.
auto tDashesStroke = test("PathStroker/dashesAreStrokedLikeAnyOtherPath") = []
{
    auto dashes = dashPath(straightLine(10.f), {{2.f, 2.f}, 0.f});
    auto region = strokeToFill(dashes, StrokeStyle {2.f, LineCap::Butt});

    check(!region.isEmpty());

    auto bounds = region.getBounds();
    check(std::abs(bounds.h - 2.f) < 0.01f, "the stroke's own width, across");
    check(std::abs(bounds.w - 10.f) < 0.01f, "and the line's length, along");
};

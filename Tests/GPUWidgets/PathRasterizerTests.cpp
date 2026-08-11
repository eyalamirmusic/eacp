#include "CoverageProbe.h"

#include <NanoTest/NanoTest.h>

#include <cmath>
#include <string>

using namespace nano;
using namespace eacp;
using namespace eacp::GPUWidgets;
using eacp::GPUWidgets::probe::rasterize;

namespace
{
using Graphics::Point;
using Graphics::Rect;

constexpr auto pi = 3.14159265358979323846f;

// A step and a half of the 8-bit channel the mask lives in: the two sides sum a
// pixel's segments in a different order, so exact equality is not on offer.
constexpr auto tolerance = 1.5f / 255.f;

// Written out again here rather than borrowed from the rasterizer, because a
// reference that shares code with what it checks checks nothing.
Vector<float> referenceSegments(const Path& path, float scale, const Rect& covered)
{
    auto left = covered.x * scale;
    auto top = covered.y * scale;
    auto segments = Vector<float> {};

    for (const auto& sub: path.getSubPaths())
    {
        const auto& points = sub.points;

        if (points.size() < 2)
            continue;

        for (auto i = 0; i < points.size(); ++i)
        {
            const auto& from = points[i];
            const auto& to = points[(i + 1) % points.size()];

            auto fromY = from.y * scale - top;
            auto toY = to.y * scale - top;

            if (fromY == toY)
                continue;

            segments.add(from.x * scale - left);
            segments.add(fromY);
            segments.add(to.x * scale - left);
            segments.add(toY);
        }
    }

    return segments;
}

float clampedIntegral(float x)
{
    auto inside = std::clamp(x, 0.f, 1.f);
    return inside * inside * 0.5f + std::max(x - 1.f, 0.f);
}

float meanClampedX(float from, float to)
{
    auto run = to - from;

    if (std::abs(run) < 1e-6f)
        return std::clamp(from, 0.f, 1.f);

    return (clampedIntegral(to) - clampedIntegral(from)) / run;
}

float referenceCoverage(const Vector<float>& segments, int x, int y, FillRule rule)
{
    auto winding = 0.f;

    for (auto i = 0; i < segments.size(); i += 4)
    {
        auto ax = segments[i] - (float) x;
        auto ay = segments[i + 1] - (float) y;
        auto bx = segments[i + 2] - (float) x;
        auto by = segments[i + 3] - (float) y;

        auto low = std::clamp(std::min(ay, by), 0.f, 1.f);
        auto high = std::clamp(std::max(ay, by), 0.f, 1.f);
        auto height = high - low;

        if (height <= 0.f)
            continue;

        auto slope = 1.f / (by - ay);
        auto xLow = ax + (low - ay) * slope * (bx - ax);
        auto xHigh = ax + (high - ay) * slope * (bx - ax);

        winding += (by > ay ? height : -height) * (1.f - meanClampedX(xLow, xHigh));
    }

    auto total = std::abs(winding);

    if (rule == FillRule::NonZero)
        return std::min(total, 1.f);

    auto folded = (total * 0.5f - std::floor(total * 0.5f)) * 2.f;
    return std::min(folded, 2.f - folded);
}

Point onCircle(const Point& centre, float radius, float angle)
{
    return {centre.x + std::cos(angle) * radius,
            centre.y + std::sin(angle) * radius};
}

// UI::Knob's indicator: two overlapping contours, so a lost segment shows as a
// seam through the join rather than as a nick in an outline.
Path knobIndicator(float size, float value)
{
    auto centre = Point {size * 0.5f, size * 0.5f};
    auto outer = size * 0.5f - 1.f;
    auto thickness = std::max(2.f, size * 0.12f);
    auto from = pi * 0.75f;
    auto to = from + pi * 1.5f * value;
    auto inner = outer - thickness;
    auto steps = 48;

    auto path = Path {};
    path.moveTo(onCircle(centre, outer, from));

    for (auto i = 1; i <= steps; ++i)
        path.lineTo(
            onCircle(centre, outer, from + (to - from) * (float) i / (float) steps));

    for (auto i = steps; i >= 0; --i)
        path.lineTo(
            onCircle(centre, inner, from + (to - from) * (float) i / (float) steps));

    path.close();

    auto pointerWidth = std::max(1.5f, size * 0.045f);
    auto tip = onCircle(centre, outer - thickness * 0.5f, to);
    auto across = Point {std::cos(to) * pointerWidth, std::sin(to) * pointerWidth};

    path.moveTo({centre.x - across.x, centre.y - across.y});
    path.lineTo({tip.x - across.x, tip.y - across.y});
    path.lineTo({tip.x + across.x, tip.y + across.y});
    path.lineTo({centre.x + across.x, centre.y + across.y});
    path.close();
    return path;
}

// Wound through itself, so the middle is covered twice and a backdrop out by one
// winding cannot hide behind non-zero saturation.
Path selfIntersectingStar(const Rect& bounds)
{
    auto centre = Point {bounds.x + bounds.w * 0.5f, bounds.y + bounds.h * 0.5f};
    auto radius = std::min(bounds.w, bounds.h) * 0.5f - 1.f;

    auto path = Path {};

    for (auto i = 0; i < 5; ++i)
    {
        auto angle = -pi * 0.5f + (float) i * 4.f * pi / 5.f;
        auto point = onCircle(centre, radius, angle);

        if (i == 0)
            path.moveTo(point);
        else
            path.lineTo(point);
    }

    path.close();
    return path;
}

// Many tile rows, two tile columns in each: binning by a segment's bounding box
// would file it under every tile of a square instead.
Path thinDiagonal(float width, float height)
{
    auto path = Path {};
    path.moveTo({2.f, 10.f});
    path.lineTo({width - 2.f, height - 2.f});
    path.lineTo({width - 2.f, height - 8.f});
    path.lineTo({2.f, 4.f});
    path.close();
    return path;
}

struct Comparison
{
    bool ran = false;
    float worst = 0.f;
    float worstExpected = 0.f;
    float worstActual = 0.f;
    int pixelsOver = 0;
    int worstX = 0;
    int worstY = 0;
    long long segmentTests = 0;
    long long unbinnedTests = 0;
};

Comparison compareAgainstReference(const Path& path, float scale, FillRule rule)
{
    auto result = Comparison {};

    if (!GPU::Device::shared().isValid())
        return result;

    auto rasterizer = PathRasterizer {};
    auto coverage = rasterize(rasterizer, path, scale, rule);

    if (coverage.empty())
        return result;

    auto width = rasterizer.getCoverageWidth();
    auto height = rasterizer.getCoverageHeight();
    auto segments = referenceSegments(path, scale, rasterizer.getCoveredBounds());

    result.ran = true;
    result.segmentTests = rasterizer.getSegmentTests();
    result.unbinnedTests = (long long) width * (long long) height
                           * (long long) rasterizer.getSegmentCount();

    for (auto y = 0; y < height; ++y)
    {
        for (auto x = 0; x < width; ++x)
        {
            auto expected = referenceCoverage(segments, x, y, rule);
            auto actual = coverage[y * width + x];
            auto difference = std::abs(actual - expected);

            if (difference > tolerance)
                ++result.pixelsOver;

            if (difference > result.worst)
            {
                result.worst = difference;
                result.worstExpected = expected;
                result.worstActual = actual;
                result.worstX = x;
                result.worstY = y;
            }
        }
    }

    return result;
}

void expectMatchesReference(const Path& path, float scale, FillRule rule)
{
    auto result = compareAgainstReference(path, scale, rule);

    if (!result.ran)
        return;

    auto where =
        std::to_string(result.pixelsOver) + " pixels past tolerance, worst at ("
        + std::to_string(result.worstX) + ", " + std::to_string(result.worstY)
        + "): expected " + std::to_string(result.worstExpected) + ", read back "
        + std::to_string(result.worstActual);

    check(result.pixelsOver == 0, where);
}
} // namespace

auto tEllipse = test("PathRasterizer/ellipseMatchesUnbinned") = []
{
    auto path = Path {};
    path.addEllipse({4.f, 4.f, 120.f, 120.f});

    expectMatchesReference(path, 1.f, FillRule::NonZero);
    expectMatchesReference(path, 2.f, FillRule::NonZero);
};

auto tRoundedRect = test("PathRasterizer/roundedRectMatchesUnbinned") = []
{
    auto path = Path {};
    path.addRoundedRect({3.f, 3.f, 200.f, 90.f}, 22.f);

    expectMatchesReference(path, 1.f, FillRule::NonZero);
};

auto tStarRules = test("PathRasterizer/selfIntersectingStarMatchesUnbinned") = []
{
    auto path = selfIntersectingStar({2.f, 2.f, 180.f, 180.f});

    expectMatchesReference(path, 1.f, FillRule::NonZero);
    expectMatchesReference(path, 1.f, FillRule::EvenOdd);
    expectMatchesReference(path, 2.f, FillRule::EvenOdd);
};

auto tKnobSizes = test("PathRasterizer/knobIndicatorMatchesUnbinnedAtEverySize") = []
{
    for (auto size: {18.f, 24.f, 33.f, 40.f, 57.f, 64.f, 96.f, 129.f, 160.f})
        expectMatchesReference(knobIndicator(size, 0.7f), 2.f, FillRule::NonZero);
};

auto tKnobValues =
    test("PathRasterizer/knobIndicatorMatchesUnbinnedThroughTravel") = []
{
    for (auto value: {0.02f, 0.25f, 0.5f, 0.75f, 1.f})
        expectMatchesReference(knobIndicator(64.f, value), 2.f, FillRule::NonZero);
};

auto tDiagonal = test("PathRasterizer/thinDiagonalMatchesUnbinned") = []
{ expectMatchesReference(thinDiagonal(240.f, 180.f), 1.f, FillRule::NonZero); };

auto tBinningCutsWork = test("PathRasterizer/binningCutsWorkWithArea") = []
{
    if (!GPU::Device::shared().isValid())
        return;

    auto path = Path {};
    path.addEllipse({0.f, 0.f, 512.f, 512.f});

    auto result = compareAgainstReference(path, 1.f, FillRule::NonZero);

    if (!result.ran)
        return;

    check(result.pixelsOver == 0);
    check(result.segmentTests > 0);
    check(result.segmentTests * 20 < result.unbinnedTests);
};

// The counting sort's array is sized to a bound and its writes are guarded, so a
// bound that came up short loses a tile's segment and softens one edge silently.
auto tEntryBoundHolds = test("PathRasterizer/theEntryBoundIsOne") = []
{
    auto paths = Vector<Path> {};
    paths.add(thinDiagonal(240.f, 180.f));
    paths.add(knobIndicator(96.f, 0.7f));
    paths.add(selfIntersectingStar({2.f, 2.f, 180.f, 180.f}));

    auto wide = Path {};
    wide.addEllipse({0.f, 0.f, 900.f, 40.f});
    paths.add(wide);

    auto tall = Path {};
    tall.addEllipse({0.f, 0.f, 40.f, 900.f});
    paths.add(tall);

    // Corner to corner, which is what the bound's "twice the rows it crosses"
    // term exists for.
    auto slash = Path {};
    slash.moveTo({4.f, 4.f});
    slash.lineTo({500.f, 400.f});
    slash.lineTo({500.f, 396.f});
    paths.add(slash);

    auto rasterizer = PathRasterizer {};

    for (const auto& path: paths)
    {
        for (auto scale: {1.f, 2.f, 3.f})
        {
            rasterizer.setScale(scale);
            rasterizer.setPath(path);

            auto counted = rasterizer.getEntryCount();
            auto reserved = rasterizer.getEntryBound();

            check(counted <= reserved,
                  std::to_string(counted) + " entries in room for "
                      + std::to_string(reserved));

            // Loose enough for a conservative derivation, tight enough that
            // reserving the whole tile grid per segment would not pass.
            check(reserved <= counted * 2 + 64,
                  std::to_string(reserved) + " reserved for "
                      + std::to_string(counted));
        }
    }
};

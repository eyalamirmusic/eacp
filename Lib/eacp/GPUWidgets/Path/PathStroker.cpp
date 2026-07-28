#include "PathStroker.h"

#include <cmath>
#include <optional>

namespace eacp::GPUWidgets
{
namespace
{
using Graphics::Point;

constexpr int maxDiscSegments = 256;

// Every piece of a stroke is a closed contour of its own and they overlap, so
// the one thing they must all agree on is which way round they wind: two that
// disagree subtract under the non-zero rule, and a join wound backwards punches
// a hole through the corner it exists to fill.
//
// The segment quad sets the convention, because it is the piece with no choice
// in the matter - its normal turns with its direction, so it comes out the same
// way round whichever way the segment points. Everything else measures itself
// against this and is reversed if it disagrees, which makes the rule impossible
// to get wrong rather than merely written down.
constexpr float houseWinding = -1.f;

float contourArea(const Vector<Point>& contour)
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

std::optional<Point> direction(const Point& from, const Point& to)
{
    auto dx = to.x - from.x;
    auto dy = to.y - from.y;
    auto length = std::sqrt(dx * dx + dy * dy);

    if (length < epsilon)
        return {};

    return Point {dx / length, dy / length};
}

// Consecutive duplicates carry no direction, so they would put a zero-length
// segment in the middle of a run and a join with no corner in it.
Vector<Point> withoutRepeats(const Vector<Point>& points, bool closed)
{
    auto cleaned = Vector<Point> {};

    for (const auto& point: points)
    {
        if (cleaned.empty() || direction(cleaned.back(), point).has_value())
            cleaned.add(point);
    }

    // A closed run's last point can also repeat its first, which is the same
    // problem at the wrap.
    while (cleaned.size() > 1 && closed
           && !direction(cleaned.back(), cleaned.front()).has_value())
        cleaned.erase(cleaned.end() - 1);

    return cleaned;
}

struct Stroker
{
    Stroker(Path& outToUse, const StrokeStyle& styleToUse, float flatnessToUse)
        : out(outToUse)
        , style(styleToUse)
        , half(styleToUse.width * 0.5f)
        , flatness(flatnessToUse)
        , discSteps(stepsForDisc(styleToUse.width * 0.5f, flatnessToUse))
    {
    }

    static int stepsForDisc(float radius, float flatness)
    {
        if (radius <= flatness)
            return 4;

        auto widest = std::acos(1.f - flatness / radius);
        return std::clamp((int) std::ceil(pi / widest), 4, maxDiscSegments);
    }

    Point normalAt(const Point& unit) const
    {
        return {-unit.y * half, unit.x * half};
    }

    void emit()
    {
        if (scratch.size() >= 3)
        {
            auto area = contourArea(scratch);

            if (std::abs(area) > epsilon)
            {
                auto forwards = (area < 0.f) == (houseWinding < 0.f);
                auto last = scratch.size() - 1;

                out.moveTo(forwards ? scratch[0] : scratch[last]);

                for (auto i = 1; i < scratch.size(); ++i)
                    out.lineTo(forwards ? scratch[i] : scratch[last - i]);

                out.close();
            }
        }

        scratch.clear();
    }

    void addSegment(const Point& a, const Point& b, const Point& normal)
    {
        scratch.add({a.x + normal.x, a.y + normal.y});
        scratch.add({b.x + normal.x, b.y + normal.y});
        scratch.add({b.x - normal.x, b.y - normal.y});
        scratch.add({a.x - normal.x, a.y - normal.y});
        emit();
    }

    // A round join and a round cap are the same disc: at an interior vertex it
    // fills the wedge and the rest of it is already inside the two quads, and at
    // an open end the half past the last point is exactly the cap.
    //
    // Pushed out by half its own sagitta for the reason Path::addEllipse is -
    // vertices placed straight on the circle make a polygon wholly inside it,
    // and a stroke reliably a little thin is worse than one whose error is
    // signed both ways.
    void addDisc(const Point& centre)
    {
        auto radius = half * (1.f + (1.f - std::cos(pi / (float) discSteps)) * 0.5f);

        for (auto i = 0; i < discSteps; ++i)
        {
            auto angle = 2.f * pi * (float) i / (float) discSteps;
            scratch.add({centre.x + std::cos(angle) * radius,
                         centre.y + std::sin(angle) * radius});
        }

        emit();
    }

    void addJoin(const Point& before, const Point& corner, const Point& after)
    {
        auto incoming = direction(before, corner);
        auto outgoing = direction(corner, after);

        if (!incoming.has_value() || !outgoing.has_value())
            return;

        auto turn = incoming->x * outgoing->y - incoming->y * outgoing->x;
        auto straightness = incoming->x * outgoing->x + incoming->y * outgoing->y;

        // How far a bevel falls short of a round join at this corner. Under the
        // flattening tolerance the two are the same shape to within the error
        // the curve already carries, so a smooth corner gets the three-point
        // join whatever was asked for - and on a flattened curve that is nearly
        // every corner, where a disc apiece would be most of the stroke.
        //
        // What this must never do is skip the corner. Two quads meeting at a
        // vertex overlap on the inside of the turn and leave a wedge on the
        // outside, and that wedge runs from the outer edge all the way down to
        // the vertex: narrow, but as deep as the stroke is wide. Eighty-four of
        // them around a circle read as spokes.
        auto bevelError =
            half * (1.f - std::sqrt(std::max(0.5f + straightness * 0.5f, 0.f)));

        auto join = bevelError < flatness ? LineJoin::Bevel : style.join;

        // The wedge opens on the side the path turns away from.
        auto side = turn > 0.f ? -1.f : 1.f;
        auto first = normalAt(*incoming);
        auto second = normalAt(*outgoing);

        auto fromCorner =
            Point {corner.x + first.x * side, corner.y + first.y * side};
        auto toCorner =
            Point {corner.x + second.x * side, corner.y + second.y * side};

        if (join == LineJoin::Round)
        {
            addDisc(corner);
            return;
        }

        scratch.add(corner);
        scratch.add(fromCorner);

        if (join == LineJoin::Miter)
        {
            if (auto tip = miterTip(corner, fromCorner, toCorner))
                scratch.add(*tip);
        }

        scratch.add(toCorner);

        // A corner that does not turn leaves a degenerate triangle, which emit()
        // drops on its area.
        emit();
    }

    // Where the two outer edges meet. Both offset points sit at half from the
    // corner, so their sum bisects the angle, and similar triangles put the tip
    // at 2*half^2 / |sum|^2 along it. Nothing is returned past the miter limit,
    // which leaves the caller's contour a bevel.
    std::optional<Point>
        miterTip(const Point& corner, const Point& from, const Point& to) const
    {
        auto sumX = (from.x - corner.x) + (to.x - corner.x);
        auto sumY = (from.y - corner.y) + (to.y - corner.y);
        auto lengthSquared = sumX * sumX + sumY * sumY;

        if (lengthSquared < epsilon)
            return {};

        auto scale = 2.f * half * half / lengthSquared;
        auto reach = std::sqrt(lengthSquared) * scale;

        if (reach > style.miterLimit * half)
            return {};

        return Point {corner.x + sumX * scale, corner.y + sumY * scale};
    }

    void addCap(const Point& end, const Point& inward)
    {
        if (style.cap == LineCap::Butt)
            return;

        if (style.cap == LineCap::Round)
        {
            addDisc(end);
            return;
        }

        auto away = direction(inward, end);

        if (!away.has_value())
            return;

        auto normal = normalAt(*away);
        auto reach = Point {away->x * half, away->y * half};

        scratch.add({end.x + normal.x, end.y + normal.y});
        scratch.add({end.x + normal.x + reach.x, end.y + normal.y + reach.y});
        scratch.add({end.x - normal.x + reach.x, end.y - normal.y + reach.y});
        scratch.add({end.x - normal.x, end.y - normal.y});
        emit();
    }

    // A sub-path that never leaves its first point: the "dot" a moveTo and a
    // zero-length lineTo mean. It has no direction, so a square cap is taken as
    // axis-aligned - which is what it is anywhere a direction cannot be had.
    void addDot(const Point& at)
    {
        if (style.cap == LineCap::Round)
        {
            addDisc(at);
            return;
        }

        if (style.cap != LineCap::Square)
            return;

        scratch.add({at.x - half, at.y - half});
        scratch.add({at.x + half, at.y - half});
        scratch.add({at.x + half, at.y + half});
        scratch.add({at.x - half, at.y + half});
        emit();
    }

    void addSubPath(const Vector<Point>& source, bool closed)
    {
        auto points = withoutRepeats(source, closed);
        auto count = points.size();

        if (count == 0)
            return;

        if (count == 1)
        {
            if (!closed)
                addDot(points[0]);

            return;
        }

        auto segments = closed ? count : count - 1;

        for (auto i = 0; i < segments; ++i)
        {
            const auto& a = points[i];
            const auto& b = points[(i + 1) % count];

            if (auto unit = direction(a, b))
                addSegment(a, b, normalAt(*unit));
        }

        // A closed run turns a corner at every vertex, including the one it
        // wraps through; an open one only between its segments, and finishes
        // with a cap instead.
        auto firstCorner = closed ? 0 : 1;
        auto lastCorner = closed ? count - 1 : count - 2;

        for (auto i = firstCorner; i <= lastCorner; ++i)
            addJoin(
                points[(i + count - 1) % count], points[i], points[(i + 1) % count]);

        if (!closed)
        {
            addCap(points[0], points[1]);
            addCap(points[count - 1], points[count - 2]);
        }
    }

    Path& out;
    const StrokeStyle& style;
    float half;
    float flatness;
    int discSteps;
    Vector<Point> scratch;
};
} // namespace

Path strokeToFill(const Path& path, const StrokeStyle& style)
{
    auto out = Path {};

    if (style.width <= 0.f || path.isEmpty())
        return out;

    out.setFlatness(path.getFlatness());

    auto stroker = Stroker {out, style, path.getFlatness()};

    for (const auto& sub: path.getSubPaths())
        stroker.addSubPath(sub.points, sub.closed);

    return out;
}
} // namespace eacp::GPUWidgets

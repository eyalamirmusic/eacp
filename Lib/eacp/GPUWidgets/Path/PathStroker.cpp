#include "PathStroker.h"

#include <algorithm>
#include <cmath>
#include <optional>

namespace eacp::GPUWidgets
{
namespace
{
using Graphics::Point;

constexpr int maxDiscSegments = 256;

// Sign of contourArea every emitted piece is reversed to match: two that
// disagree would subtract under the non-zero rule. Set by the segment quad,
// which winds the same way whichever direction its segment points.
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

// Duplicates carry no direction, leaving zero-length segments and cornerless
// joins.
Vector<Point> withoutRepeats(const Vector<Point>& points, bool closed)
{
    auto cleaned = Vector<Point> {};

    for (const auto& point: points)
    {
        if (cleaned.empty() || direction(cleaned.back(), point).has_value())
            cleaned.add(point);
    }

    // The same problem at the wrap.
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

    // Serves as both round join and round cap. Outset by half its sagitta so it
    // straddles the true circle rather than sitting inside it.
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

        // How far a bevel falls short of a round join here. Below the flattening
        // tolerance the two are indistinguishable, so a smooth corner is beveled
        // whatever was asked for - but never skipped, which would leave a wedge.
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

        // emit() drops the degenerate triangle a corner that does not turn makes.
        emit();
    }

    // Where the two outer edges meet, or nothing past the miter limit - which
    // leaves the caller's contour a bevel.
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

    // A sub-path with no direction, so a square cap is taken as axis-aligned.
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

        // A closed run also turns a corner at the vertex it wraps through.
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

namespace
{
struct DashCursor
{
    void advanceTo(int entry, const Vector<float>& pattern)
    {
        index = entry % pattern.size();
        remaining = pattern[index];

        // Derived, not toggled, so skipping an entry cannot desynchronise it.
        drawing = index % 2 == 0;
    }

    // Steps over zero-length entries, so a zero-length on draws nothing rather
    // than the dot the format asks for.
    void advance(const Vector<float>& pattern)
    {
        for (auto steps = 0; steps < pattern.size(); ++steps)
        {
            advanceTo(index + 1, pattern);

            if (remaining > 0.f)
                return;
        }
    }

    int index = 0;
    float remaining = 0.f;
    bool drawing = true;
};

float totalLength(const Vector<float>& pattern)
{
    auto total = 0.f;

    for (auto length: pattern)
        total += length;

    return total;
}

// The offset belongs to the pattern, so every sub-path starts at the same place.
DashCursor cursorAt(const Vector<float>& pattern, float offset)
{
    auto total = totalLength(pattern);
    auto into = std::fmod(offset, total);

    if (into < 0.f)
        into += total;

    auto cursor = DashCursor {};
    cursor.advanceTo(0, pattern);

    while (into >= cursor.remaining)
    {
        into -= cursor.remaining;
        cursor.advanceTo(cursor.index + 1, pattern);
    }

    cursor.remaining -= into;

    return cursor;
}

Point along(const Point& from, const Point& to, float fraction)
{
    return {from.x + (to.x - from.x) * fraction,
            from.y + (to.y - from.y) * fraction};
}
} // namespace

bool DashPattern::isEmpty() const
{
    if (lengths.empty())
        return true;

    auto total = 0.f;

    for (auto length: lengths)
    {
        if (length < 0.f)
            return true;

        total += length;
    }

    return total <= 0.f;
}

Path dashPath(const Path& path, const DashPattern& dash)
{
    if (dash.isEmpty() || path.isEmpty())
        return path;

    auto pattern = dash.lengths;

    // An odd list repeats to become even, so "5 3 2" runs 5 on, 3 off, 2 on,
    // 5 off, 3 on, 2 off.
    if (dash.lengths.size() % 2 != 0)
        for (auto length: dash.lengths)
            pattern.add(length);

    auto out = Path {};
    out.setFlatness(path.getFlatness());

    for (const auto& sub: path.getSubPaths())
    {
        if (sub.points.size() < 2)
            continue;

        auto cursor = cursorAt(pattern, dash.offset);
        auto penDown = false;

        auto walk = [&](const Point& from, const Point& to)
        {
            auto span = std::sqrt((to.x - from.x) * (to.x - from.x)
                                  + (to.y - from.y) * (to.y - from.y));

            if (span < epsilon)
                return;

            auto travelled = 0.f;

            while (travelled < span)
            {
                auto step = std::min(cursor.remaining, span - travelled);

                if (cursor.drawing)
                {
                    if (!penDown)
                    {
                        out.moveTo(along(from, to, travelled / span));
                        penDown = true;
                    }

                    out.lineTo(along(from, to, (travelled + step) / span));
                }

                travelled += step;
                cursor.remaining -= step;

                if (cursor.remaining > 0.f)
                    continue;

                cursor.advance(pattern);
                penDown = false;
            }
        };

        for (auto i = 1; i < sub.points.size(); ++i)
            walk(sub.points[i - 1], sub.points[i]);

        // The closing edge dashes like any other segment.
        if (sub.closed)
            walk(sub.points.back(), sub.points.front());
    }

    return out;
}

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

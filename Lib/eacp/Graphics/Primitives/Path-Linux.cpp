#include "Path-Linux.h"

// The Linux Path: geometry, and nothing else.
//
// The Apple and Windows files hand their points straight to a CGPath or an
// ID2D1PathGeometry, both of which are a renderer's idea of a path. Linux has
// no 2D context to hand them to — plan stage 6 decides whether it ever grows
// one or draws this tier through the GPU coverage rasterizer instead — so here
// the record *is* the implementation. That is already enough for everything
// that measures or tessellates rather than rasterizes, which is what
// getHandle() exists for.
//
// The record is a faithful transcript: a lineTo with no moveTo before it is
// stored as written rather than being given an implied start point, because
// the two shipping backends disagree about what that start point is (Direct2D
// opens a figure at the last point, Core Graphics treats it as undefined) and
// guessing here would bake one of them in.

namespace eacp::Graphics
{
namespace
{
// How far along the tangent a quarter circle's cubic control points sit, as a
// fraction of the radius: 4/3 * (sqrt(2) - 1).
constexpr auto pathQuarterArcControl = 0.5522847498307933f;
} // namespace

struct Path::Native
{
    PathGeometry geometry;
};

int PathCommand::pointCount() const
{
    switch (verb)
    {
        case Verb::move:
        case Verb::line:
            return 1;
        case Verb::quad:
            return 2;
        case Verb::cubic:
            return 3;
        case Verb::close:
            break;
    }

    return 0;
}

Path::Path()
    : impl()
{
}

void Path::clear()
{
    impl->geometry.commands.clear();
}

void Path::moveTo(const Point& target)
{
    impl->geometry.commands.add({PathCommand::Verb::move, {target}});
}

void Path::lineTo(const Point& target)
{
    impl->geometry.commands.add({PathCommand::Verb::line, {target}});
}

void Path::quadTo(float cx, float cy, float x, float y)
{
    impl->geometry.commands.add({PathCommand::Verb::quad, {{cx, cy}, {x, y}}});
}

void Path::cubicTo(float c1x, float c1y, float c2x, float c2y, float x, float y)
{
    impl->geometry.commands.add(
        {PathCommand::Verb::cubic, {{c1x, c1y}, {c2x, c2y}, {x, y}}});
}

void Path::close()
{
    impl->geometry.commands.add({PathCommand::Verb::close});
}

void Path::addRect(const Rect& rect)
{
    moveTo({rect.left(), rect.top()});
    lineTo({rect.right(), rect.top()});
    lineTo({rect.right(), rect.bottom()});
    lineTo({rect.left(), rect.bottom()});
    close();
}

void Path::addRoundedRect(const Rect& rect, float radius)
{
    auto r = clampedCornerRadius(rect, radius);
    auto pull = r * pathQuarterArcControl;

    auto left = rect.left();
    auto right = rect.right();
    auto top = rect.top();
    auto bottom = rect.bottom();

    moveTo({left + r, top});
    lineTo({right - r, top});
    cubicTo(right - r + pull, top, right, top + r - pull, right, top + r);
    lineTo({right, bottom - r});
    cubicTo(right, bottom - r + pull, right - r + pull, bottom, right - r, bottom);
    lineTo({left + r, bottom});
    cubicTo(left + r - pull, bottom, left, bottom - r + pull, left, bottom - r);
    lineTo({left, top + r});
    cubicTo(left, top + r - pull, left + r - pull, top, left + r, top);
    close();
}

void Path::addEllipse(const Rect& rect)
{
    auto center = rect.center();
    auto rx = rect.w / 2.f;
    auto ry = rect.h / 2.f;
    auto pullX = rx * pathQuarterArcControl;
    auto pullY = ry * pathQuarterArcControl;

    auto x = center.x;
    auto y = center.y;

    moveTo({x + rx, y});
    cubicTo(x + rx, y + pullY, x + pullX, y + ry, x, y + ry);
    cubicTo(x - pullX, y + ry, x - rx, y + pullY, x - rx, y);
    cubicTo(x - rx, y - pullY, x - pullX, y - ry, x, y - ry);
    cubicTo(x + pullX, y - ry, x + rx, y - pullY, x + rx, y);
    close();
}

Path Path::scaled(float sx, float sy) const
{
    auto result = Path();

    for (auto command: impl->geometry.commands)
    {
        for (auto i = 0; i < command.pointCount(); ++i)
        {
            command.points[i].x *= sx;
            command.points[i].y *= sy;
        }

        result.impl->geometry.commands.add(command);
    }

    return result;
}

void* Path::getHandle() const
{
    return const_cast<PathGeometry*>(&impl->geometry);
}

const PathGeometry& getPathGeometry(const Path& path)
{
    return *static_cast<const PathGeometry*>(path.getHandle());
}

} // namespace eacp::Graphics

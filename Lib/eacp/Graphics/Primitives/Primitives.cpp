#include "Primitives.h"

namespace eacp::Graphics
{
Point::Point(float xToUse, float yToUse)
    : x(xToUse)
    , y(yToUse)
{
}

Rect::Rect(float xToUse, float yToUse, float wToUse, float hToUse)
    : x(xToUse)
    , y(yToUse)
    , w(wToUse)
    , h(hToUse)
{
}

Rect Rect::getRelative(const Rect& ratio) const
{
    return {x + (w * ratio.x), y + (h * ratio.y), w * ratio.w, h * ratio.h};
}

Point Rect::getRelativePoint(const Point& point) const
{
    return {(point.x - x) / w, (point.y - y) / h};
}

bool Rect::contains(const Point& point) const
{
    return point.x >= x && point.x < x + w && point.y >= y && point.y < y + h;
}

Color::Color(float rToUse, float gToUse, float bToUse, float aToUse)
    : r(rToUse)
    , g(gToUse)
    , b(bToUse)
    , a(aToUse)
{
}

Color Color::withAlpha(float alpha) const
{
    auto copy = *this;
    copy.a = alpha;
    return copy;
}

Point operator+(const Point& a, const Point& b)
{
    return {a.x + b.x, a.y + b.y};
}

Point operator-(const Point& a, const Point& b)
{
    return {a.x - b.x, a.y - b.y};
}

LinearGradient::LinearGradient(Point startToUse, Point endToUse,
                               std::initializer_list<GradientStop> stopsToUse)
    : start(startToUse)
    , end(endToUse)
    , stops(stopsToUse)
{
}

}
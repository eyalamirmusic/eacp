#include "Primitives.h"
#include <algorithm>
#include <cmath>

namespace eacp::Graphics
{
Point::Point(float xToUse, float yToUse)
    : x(xToUse)
    , y(yToUse)
{
}

float Point::length() const
{
    return std::sqrt(x * x + y * y);
}

float Point::distanceTo(const Point& other) const
{
    return (*this - other).length();
}

Point Point::normalized() const
{
    auto len = length();
    if (len == 0.f)
        return {0.f, 0.f};
    return {x / len, y / len};
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

Rect Rect::inset(float amount) const
{
    return {x + amount, y + amount, w - amount * 2, h - amount * 2};
}

Rect Rect::inset(float horizontal, float vertical) const
{
    return {x + horizontal, y + vertical, w - horizontal * 2, h - vertical * 2};
}

Rect Rect::withX(float newX) const
{
    return {newX, y, w, h};
}

Rect Rect::withY(float newY) const
{
    return {x, newY, w, h};
}

Rect Rect::withWidth(float newW) const
{
    return {x, y, newW, h};
}

Rect Rect::withHeight(float newH) const
{
    return {x, y, w, newH};
}

Rect Rect::withPosition(float newX, float newY) const
{
    return {newX, newY, w, h};
}

Rect Rect::withSize(float newW, float newH) const
{
    return {x, y, newW, newH};
}

Rect Rect::fromLeft(float width, float margin) const
{
    return {x + margin, y, width, h};
}

Rect Rect::fromRight(float width, float margin) const
{
    return {x + w - width - margin, y, width, h};
}

Rect Rect::fromTop(float height, float margin) const
{
    return {x, y + h - height - margin, w, height};
}

Rect Rect::fromBottom(float height, float margin) const
{
    return {x, y + margin, w, height};
}

Rect Rect::removeFromLeft(float amount)
{
    auto removed = Rect {x, y, amount, h};
    x += amount;
    w -= amount;
    return removed;
}

Rect Rect::removeFromRight(float amount)
{
    w -= amount;
    return {x + w, y, amount, h};
}

Rect Rect::removeFromTop(float amount)
{
    h -= amount;
    return {x, y + h, w, amount};
}

Rect Rect::removeFromBottom(float amount)
{
    auto removed = Rect {x, y, w, amount};
    y += amount;
    h -= amount;
    return removed;
}

Point Rect::center() const
{
    return {x + w / 2.f, y + h / 2.f};
}

float Rect::right() const
{
    return x + w;
}

float Rect::top() const
{
    return y + h;
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
    return {r, g, b, alpha};
}

Color Color::gray(float value, float alpha)
{
    return {value, value, value, alpha};
}

Color Color::white(float alpha)
{
    return {1.f, 1.f, 1.f, alpha};
}

Color Color::black(float alpha)
{
    return {0.f, 0.f, 0.f, alpha};
}

Color Color::brighter(float amount) const
{
    return {std::min(r + amount, 1.f),
            std::min(g + amount, 1.f),
            std::min(b + amount, 1.f),
            a};
}

Color Color::darker(float amount) const
{
    return {std::max(r - amount, 0.f),
            std::max(g - amount, 0.f),
            std::max(b - amount, 0.f),
            a};
}

Point operator+(const Point& a, const Point& b)
{
    return {a.x + b.x, a.y + b.y};
}

Point operator-(const Point& a, const Point& b)
{
    return {a.x - b.x, a.y - b.y};
}

LinearGradient::LinearGradient(Point startToUse,
                               Point endToUse,
                               std::initializer_list<GradientStop> stopsToUse)
    : start(startToUse)
    , end(endToUse)
    , stops(stopsToUse)
{
}

} // namespace eacp::Graphics
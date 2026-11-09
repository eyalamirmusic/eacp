#pragma once

#include <algorithm>
#include <cmath>

namespace eacp::Geometry
{
template <typename T>
struct Point
{
    constexpr Point() = default;

    constexpr Point(T xToUse, T yToUse)
        : x(xToUse)
        , y(yToUse)
    {
    }

    T length() const { return static_cast<T>(std::sqrt(x * x + y * y)); }

    T distanceTo(const Point& other) const { return (*this - other).length(); }

    Point normalized() const
    {
        auto len = length();
        if (len == T {})
            return {T {}, T {}};
        return {x / len, y / len};
    }

    template <typename U>
    constexpr Point<U> to() const
    {
        return {static_cast<U>(x), static_cast<U>(y)};
    }

    friend constexpr Point operator+(const Point& a, const Point& b)
    {
        return {a.x + b.x, a.y + b.y};
    }

    friend constexpr Point operator-(const Point& a, const Point& b)
    {
        return {a.x - b.x, a.y - b.y};
    }

    T x = T {};
    T y = T {};
};

// A border of four thicknesses, one per edge, in points. Every side defaults
// to zero so a caller names only the edges it means: `Insets {.top = 56.f}`.
template <typename T>
struct Insets
{
    // The width and height the four sides add up to.
    constexpr Point<T> size() const { return {left + right, top + bottom}; }

    T top = T {};
    T left = T {};
    T bottom = T {};
    T right = T {};
};

// An axis-aligned rectangle in a **y-down** space: y grows downward, so y = 0
// is the top edge and `fromTop` returns the slice against it.
//
// That is what the rest of eacp means by y. The backing views set isFlipped, so
// View::setBounds and MouseEvent::pos are both measured from the top, and the
// sprite, glyph and scissor paths all map y = 0 to the top of their target. The
// splitters below came from JUCE, where views really are y-up, and keeping that
// sense here meant `removeFromTop` handed you the bottom slice — a header laid
// out with it landed along the bottom edge of its window, which is silent
// because it still compiles and still draws something.
template <typename T>
struct Rectangle
{
    constexpr Rectangle() = default;

    constexpr Rectangle(T xToUse, T yToUse, T wToUse, T hToUse)
        : x(xToUse)
        , y(yToUse)
        , w(wToUse)
        , h(hToUse)
    {
    }

    constexpr Rectangle getRelative(const Rectangle& ratio) const
    {
        return {x + (w * ratio.x), y + (h * ratio.y), w * ratio.w, h * ratio.h};
    }

    constexpr Point<T> getRelativePoint(const Point<T>& point) const
    {
        return {(point.x - x) / w, (point.y - y) / h};
    }

    constexpr bool contains(const Point<T>& point) const
    {
        return point.x >= x && point.x < x + w && point.y >= y && point.y < y + h;
    }

    constexpr bool contains(const Rectangle& other) const
    {
        return other.x >= x && other.y >= y && other.right() <= right()
               && other.bottom() <= bottom();
    }

    // A rect with no area. Both an explicitly empty rect and one whose width or
    // height has gone negative — which is what oversized insets and splitters
    // produce — count, so callers can test the result of either.
    constexpr bool isEmpty() const { return w <= T {} || h <= T {}; }

    constexpr bool intersects(const Rectangle& other) const
    {
        return !isEmpty() && !other.isEmpty() && x < other.right()
               && other.x < right() && y < other.bottom() && other.y < bottom();
    }

    // The overlap, or an empty rect when there is none. Nesting clip regions is
    // the reason this exists: the GPU has exactly one scissor rect and no stack,
    // so a child's clip has to be intersected with its parent's on the way down
    // rather than replacing it.
    constexpr Rectangle intersection(const Rectangle& other) const
    {
        const auto newX = std::max(x, other.x);
        const auto newY = std::max(y, other.y);
        const auto newRight = std::min(right(), other.right());
        const auto newBottom = std::min(bottom(), other.bottom());

        // Disjoint rects give a negative extent here. Collapsed to zero so the
        // result is an ordinary empty rect rather than one that draws inside out.
        return {newX,
                newY,
                std::max(newRight - newX, T {}),
                std::max(newBottom - newY, T {})};
    }

    constexpr Rectangle inset(T amount) const
    {
        return {x + amount, y + amount, w - amount * 2, h - amount * 2};
    }

    constexpr Rectangle inset(T horizontal, T vertical) const
    {
        return {x + horizontal, y + vertical, w - horizontal * 2, h - vertical * 2};
    }

    constexpr Rectangle inset(const Insets<T>& insets) const
    {
        return {x + insets.left,
                y + insets.top,
                w - insets.left - insets.right,
                h - insets.top - insets.bottom};
    }

    constexpr Rectangle withX(T newX) const { return {newX, y, w, h}; }
    constexpr Rectangle withY(T newY) const { return {x, newY, w, h}; }
    constexpr Rectangle withWidth(T newW) const { return {x, y, newW, h}; }
    constexpr Rectangle withHeight(T newH) const { return {x, y, w, newH}; }
    constexpr Rectangle withPosition(T newX, T newY) const
    {
        return {newX, newY, w, h};
    }
    constexpr Rectangle withSize(T newW, T newH) const { return {x, y, newW, newH}; }

    constexpr Rectangle fromLeft(T width, T margin = T {}) const
    {
        return {x + margin, y, width, h};
    }

    constexpr Rectangle fromRight(T width, T margin = T {}) const
    {
        return {x + w - width - margin, y, width, h};
    }

    constexpr Rectangle fromTop(T height, T margin = T {}) const
    {
        return {x, y + margin, w, height};
    }

    constexpr Rectangle fromBottom(T height, T margin = T {}) const
    {
        return {x, y + h - height - margin, w, height};
    }

    constexpr Rectangle removeFromLeft(T amount)
    {
        auto removed = Rectangle {x, y, amount, h};
        x += amount;
        w -= amount;
        return removed;
    }

    constexpr Rectangle removeFromRight(T amount)
    {
        w -= amount;
        return {x + w, y, amount, h};
    }

    constexpr Rectangle removeFromTop(T amount)
    {
        auto removed = Rectangle {x, y, w, amount};
        y += amount;
        h -= amount;
        return removed;
    }

    constexpr Rectangle removeFromBottom(T amount)
    {
        h -= amount;
        return {x, y + h, w, amount};
    }

    constexpr Point<T> center() const { return {x + w / T {2}, y + h / T {2}}; }

    // Edge coordinates. `top` is the smaller y and `bottom` the larger, so
    // top <= bottom for any rect with a non-negative height.
    constexpr T left() const { return x; }
    constexpr T right() const { return x + w; }
    constexpr T top() const { return y; }
    constexpr T bottom() const { return y + h; }

    template <typename U>
    constexpr Rectangle<U> to() const
    {
        return {static_cast<U>(x),
                static_cast<U>(y),
                static_cast<U>(w),
                static_cast<U>(h)};
    }

    // The smallest rectangle of whole U units that covers this one: left and
    // top floored, right and bottom ceiled.
    template <typename U>
    Rectangle<U> roundedOutwardTo() const
    {
        const auto roundedLeft = static_cast<U>(std::floor(left()));
        const auto roundedTop = static_cast<U>(std::floor(top()));
        const auto roundedRight = static_cast<U>(std::ceil(right()));
        const auto roundedBottom = static_cast<U>(std::ceil(bottom()));

        return {roundedLeft,
                roundedTop,
                roundedRight - roundedLeft,
                roundedBottom - roundedTop};
    }

    T x = T {};
    T y = T {};
    T w = T {};
    T h = T {};
};
} // namespace eacp::Geometry

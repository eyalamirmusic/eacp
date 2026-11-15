#pragma once

namespace eacp::Geometry
{
template <typename T>
struct Point
{
    Point() = default;

    Point(T xToUse, T yToUse);

    // For int, the length and the unit vector are rounded to the nearest whole
    // value.
    T length() const;
    T distanceTo(const Point& other) const;
    Point normalized() const;

    template <typename U>
    Point<U> to() const;

    Point operator+(const Point& other) const;
    Point operator-(const Point& other) const;

    bool operator==(const Point&) const = default;

    T x = T {};
    T y = T {};
};

// A border of four thicknesses, one per edge, in points. Every side defaults
// to zero so a caller names only the edges it means: `Insets {.top = 56.f}`.
template <typename T>
struct Insets
{
    // The width and height the four sides add up to.
    Point<T> size() const;

    bool operator==(const Insets&) const = default;

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
    Rectangle() = default;

    Rectangle(T xToUse, T yToUse, T wToUse, T hToUse);

    Rectangle getRelative(const Rectangle& ratio) const;

    // For int, an axis with no extent maps every point to 0 rather than
    // dividing by zero.
    Point<T> getRelativePoint(const Point<T>& point) const;

    bool contains(const Point<T>& point) const;
    bool contains(const Rectangle& other) const;

    // A rect with no area. Both an explicitly empty rect and one whose width or
    // height has gone negative — which is what oversized insets and splitters
    // produce — count, so callers can test the result of either.
    bool isEmpty() const;

    bool intersects(const Rectangle& other) const;

    // The overlap, or an empty rect when there is none. Nesting clip regions is
    // the reason this exists: the GPU has exactly one scissor rect and no stack,
    // so a child's clip has to be intersected with its parent's on the way down
    // rather than replacing it.
    Rectangle intersection(const Rectangle& other) const;

    Rectangle inset(T amount) const;
    Rectangle inset(T horizontal, T vertical) const;
    Rectangle inset(const Insets<T>& insets) const;

    Rectangle withX(T newX) const;
    Rectangle withY(T newY) const;
    Rectangle withWidth(T newW) const;
    Rectangle withHeight(T newH) const;
    Rectangle withPosition(T newX, T newY) const;
    Rectangle withSize(T newW, T newH) const;

    Rectangle fromLeft(T width, T margin = T {}) const;
    Rectangle fromRight(T width, T margin = T {}) const;
    Rectangle fromTop(T height, T margin = T {}) const;
    Rectangle fromBottom(T height, T margin = T {}) const;

    Rectangle removeFromLeft(T amount);
    Rectangle removeFromRight(T amount);
    Rectangle removeFromTop(T amount);
    Rectangle removeFromBottom(T amount);

    // For int, the half extents are truncated toward zero.
    Point<T> center() const;

    // Edge coordinates. `top` is the smaller y and `bottom` the larger, so
    // top <= bottom for any rect with a non-negative height.
    T left() const;
    T right() const;
    T top() const;
    T bottom() const;

    template <typename U>
    Rectangle<U> to() const;

    // The smallest rectangle of whole U units that covers this one: left and
    // top floored, right and bottom ceiled.
    template <typename U>
    Rectangle<U> roundedOutwardTo() const;

    bool operator==(const Rectangle&) const = default;

    T x = T {};
    T y = T {};
    T w = T {};
    T h = T {};
};

extern template struct Point<float>;
extern template struct Point<double>;
extern template struct Point<int>;

extern template struct Insets<float>;
extern template struct Insets<double>;
extern template struct Insets<int>;

extern template struct Rectangle<float>;
extern template struct Rectangle<double>;
extern template struct Rectangle<int>;
} // namespace eacp::Geometry

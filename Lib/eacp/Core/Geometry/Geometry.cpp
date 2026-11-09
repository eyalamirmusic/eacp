#include "Geometry.h"

#include <algorithm>
#include <cmath>
#include <type_traits>

namespace eacp::Geometry
{
namespace
{
int nearestInt(double value)
{
    return static_cast<int>(std::lround(value));
}

template <typename T>
T ratioOf(T numerator, T denominator)
{
    if constexpr (std::is_integral_v<T>)
    {
        if (denominator == T {})
            return T {};
    }

    return numerator / denominator;
}

template <typename U, typename T>
U floorTo(T value)
{
    if constexpr (std::is_integral_v<T>)
        return static_cast<U>(value);
    else
        return static_cast<U>(std::floor(value));
}

template <typename U, typename T>
U ceilTo(T value)
{
    if constexpr (std::is_integral_v<T>)
        return static_cast<U>(value);
    else
        return static_cast<U>(std::ceil(value));
}
} // namespace

template <typename T>
template <typename U>
Point<U> Point<T>::to() const
{
    return {static_cast<U>(x), static_cast<U>(y)};
}

template <typename T>
Point<T>::Point(T xToUse, T yToUse)
    : x(xToUse)
    , y(yToUse)
{
}

template <typename T>
T Point<T>::length() const
{
    if constexpr (std::is_integral_v<T>)
        return nearestInt(
            std::hypot(static_cast<double>(x), static_cast<double>(y)));
    else
        return std::sqrt(x * x + y * y);
}

template <typename T>
T Point<T>::distanceTo(const Point& other) const
{
    return (*this - other).length();
}

template <typename T>
Point<T> Point<T>::normalized() const
{
    if constexpr (std::is_integral_v<T>)
    {
        auto unit = to<double>().normalized();
        return {nearestInt(unit.x), nearestInt(unit.y)};
    }
    else
    {
        auto len = length();

        if (len == T {})
            return {};

        return {x / len, y / len};
    }
}

template <typename T>
Point<T> Point<T>::operator+(const Point& other) const
{
    return {x + other.x, y + other.y};
}

template <typename T>
Point<T> Point<T>::operator-(const Point& other) const
{
    return {x - other.x, y - other.y};
}

template <typename T>
Point<T> Insets<T>::size() const
{
    return {left + right, top + bottom};
}

template <typename T>
Rectangle<T>::Rectangle(T xToUse, T yToUse, T wToUse, T hToUse)
    : x(xToUse)
    , y(yToUse)
    , w(wToUse)
    , h(hToUse)
{
}

template <typename T>
Rectangle<T> Rectangle<T>::getRelative(const Rectangle& ratio) const
{
    return {x + (w * ratio.x), y + (h * ratio.y), w * ratio.w, h * ratio.h};
}

template <typename T>
Point<T> Rectangle<T>::getRelativePoint(const Point<T>& point) const
{
    return {ratioOf(point.x - x, w), ratioOf(point.y - y, h)};
}

template <typename T>
bool Rectangle<T>::contains(const Point<T>& point) const
{
    return point.x >= x && point.x < right() && point.y >= y && point.y < bottom();
}

template <typename T>
bool Rectangle<T>::contains(const Rectangle& other) const
{
    return other.x >= x && other.y >= y && other.right() <= right()
           && other.bottom() <= bottom();
}

template <typename T>
bool Rectangle<T>::isEmpty() const
{
    return w <= T {} || h <= T {};
}

template <typename T>
bool Rectangle<T>::intersects(const Rectangle& other) const
{
    return !isEmpty() && !other.isEmpty() && x < other.right() && other.x < right()
           && y < other.bottom() && other.y < bottom();
}

template <typename T>
Rectangle<T> Rectangle<T>::intersection(const Rectangle& other) const
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

template <typename T>
Rectangle<T> Rectangle<T>::inset(T amount) const
{
    return inset(amount, amount);
}

template <typename T>
Rectangle<T> Rectangle<T>::inset(T horizontal, T vertical) const
{
    return {x + horizontal, y + vertical, w - horizontal * 2, h - vertical * 2};
}

template <typename T>
Rectangle<T> Rectangle<T>::inset(const Insets<T>& insets) const
{
    return {x + insets.left,
            y + insets.top,
            w - insets.left - insets.right,
            h - insets.top - insets.bottom};
}

template <typename T>
Rectangle<T> Rectangle<T>::withX(T newX) const
{
    return {newX, y, w, h};
}

template <typename T>
Rectangle<T> Rectangle<T>::withY(T newY) const
{
    return {x, newY, w, h};
}

template <typename T>
Rectangle<T> Rectangle<T>::withWidth(T newW) const
{
    return {x, y, newW, h};
}

template <typename T>
Rectangle<T> Rectangle<T>::withHeight(T newH) const
{
    return {x, y, w, newH};
}

template <typename T>
Rectangle<T> Rectangle<T>::withPosition(T newX, T newY) const
{
    return {newX, newY, w, h};
}

template <typename T>
Rectangle<T> Rectangle<T>::withSize(T newW, T newH) const
{
    return {x, y, newW, newH};
}

template <typename T>
Rectangle<T> Rectangle<T>::fromLeft(T width, T margin) const
{
    return {x + margin, y, width, h};
}

template <typename T>
Rectangle<T> Rectangle<T>::fromRight(T width, T margin) const
{
    return {x + w - width - margin, y, width, h};
}

template <typename T>
Rectangle<T> Rectangle<T>::fromTop(T height, T margin) const
{
    return {x, y + margin, w, height};
}

template <typename T>
Rectangle<T> Rectangle<T>::fromBottom(T height, T margin) const
{
    return {x, y + h - height - margin, w, height};
}

template <typename T>
Rectangle<T> Rectangle<T>::removeFromLeft(T amount)
{
    auto removed = Rectangle {x, y, amount, h};
    x += amount;
    w -= amount;
    return removed;
}

template <typename T>
Rectangle<T> Rectangle<T>::removeFromRight(T amount)
{
    w -= amount;
    return {x + w, y, amount, h};
}

template <typename T>
Rectangle<T> Rectangle<T>::removeFromTop(T amount)
{
    auto removed = Rectangle {x, y, w, amount};
    y += amount;
    h -= amount;
    return removed;
}

template <typename T>
Rectangle<T> Rectangle<T>::removeFromBottom(T amount)
{
    h -= amount;
    return {x, y + h, w, amount};
}

template <typename T>
Point<T> Rectangle<T>::center() const
{
    return {x + w / T {2}, y + h / T {2}};
}

template <typename T>
T Rectangle<T>::left() const
{
    return x;
}

template <typename T>
T Rectangle<T>::right() const
{
    return x + w;
}

template <typename T>
T Rectangle<T>::top() const
{
    return y;
}

template <typename T>
T Rectangle<T>::bottom() const
{
    return y + h;
}

template <typename T>
template <typename U>
Rectangle<U> Rectangle<T>::to() const
{
    return {
        static_cast<U>(x), static_cast<U>(y), static_cast<U>(w), static_cast<U>(h)};
}

template <typename T>
template <typename U>
Rectangle<U> Rectangle<T>::roundedOutwardTo() const
{
    const auto roundedLeft = floorTo<U>(left());
    const auto roundedTop = floorTo<U>(top());
    const auto roundedRight = ceilTo<U>(right());
    const auto roundedBottom = ceilTo<U>(bottom());

    return {roundedLeft,
            roundedTop,
            roundedRight - roundedLeft,
            roundedBottom - roundedTop};
}

template struct Point<float>;
template struct Point<double>;
template struct Point<int>;

template struct Insets<float>;
template struct Insets<double>;
template struct Insets<int>;

template struct Rectangle<float>;
template struct Rectangle<double>;
template struct Rectangle<int>;

template Point<float> Point<float>::to<float>() const;
template Point<double> Point<float>::to<double>() const;
template Point<int> Point<float>::to<int>() const;
template Point<float> Point<double>::to<float>() const;
template Point<double> Point<double>::to<double>() const;
template Point<int> Point<double>::to<int>() const;
template Point<float> Point<int>::to<float>() const;
template Point<double> Point<int>::to<double>() const;
template Point<int> Point<int>::to<int>() const;

template Rectangle<float> Rectangle<float>::to<float>() const;
template Rectangle<double> Rectangle<float>::to<double>() const;
template Rectangle<int> Rectangle<float>::to<int>() const;
template Rectangle<float> Rectangle<double>::to<float>() const;
template Rectangle<double> Rectangle<double>::to<double>() const;
template Rectangle<int> Rectangle<double>::to<int>() const;
template Rectangle<float> Rectangle<int>::to<float>() const;
template Rectangle<double> Rectangle<int>::to<double>() const;
template Rectangle<int> Rectangle<int>::to<int>() const;

template Rectangle<float> Rectangle<float>::roundedOutwardTo<float>() const;
template Rectangle<double> Rectangle<float>::roundedOutwardTo<double>() const;
template Rectangle<int> Rectangle<float>::roundedOutwardTo<int>() const;
template Rectangle<float> Rectangle<double>::roundedOutwardTo<float>() const;
template Rectangle<double> Rectangle<double>::roundedOutwardTo<double>() const;
template Rectangle<int> Rectangle<double>::roundedOutwardTo<int>() const;
template Rectangle<float> Rectangle<int>::roundedOutwardTo<float>() const;
template Rectangle<double> Rectangle<int>::roundedOutwardTo<double>() const;
template Rectangle<int> Rectangle<int>::roundedOutwardTo<int>() const;
} // namespace eacp::Geometry

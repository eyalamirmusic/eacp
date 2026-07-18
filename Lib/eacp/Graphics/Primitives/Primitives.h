#pragma once

#include "../Common.h"

#include <algorithm>
#include <initializer_list>

namespace eacp::Graphics
{
struct Point
{
    Point() = default;
    Point(float xToUse, float yToUse);

    float length() const;
    float distanceTo(const Point& other) const;
    Point normalized() const;

    float x = 0.f;
    float y = 0.f;
};

Point operator+(const Point& a, const Point& b);

Point operator-(const Point& a, const Point& b);

struct Rect
{
    Rect() = default;
    Rect(float xToUse, float yToUse, float wToUse, float hToUse);

    Rect getRelative(const Rect& ratio) const;
    Point getRelativePoint(const Point& point) const;

    bool contains(const Point& point) const;

    Rect inset(float amount) const;
    Rect inset(float horizontal, float vertical) const;

    Rect withX(float newX) const;
    Rect withY(float newY) const;
    Rect withWidth(float newW) const;
    Rect withHeight(float newH) const;
    Rect withPosition(float newX, float newY) const;
    Rect withSize(float newW, float newH) const;

    Rect fromLeft(float width, float margin = 0.f) const;
    Rect fromRight(float width, float margin = 0.f) const;
    Rect fromTop(float height, float margin = 0.f) const;
    Rect fromBottom(float height, float margin = 0.f) const;

    Rect removeFromLeft(float amount);
    Rect removeFromRight(float amount);
    Rect removeFromTop(float amount);
    Rect removeFromBottom(float amount);

    Point center() const;
    float right() const;
    float top() const;

    float x = 0.f;
    float y = 0.f;
    float w = 0.f;
    float h = 0.f;
};

// Corners bigger than the rect they round are out of contract for the platform
// path builders, so every rounded-rect call site fits the radius first.
float clampedCornerRadius(const Rect& rect, float radius);

// Defined inline and constexpr so themes can be compile-time constants: a
// palette is a table of named colors, and building one at static-init time
// costs nothing and lets it live in rodata.
struct Color
{
    constexpr Color() = default;

    constexpr Color(float rToUse, float gToUse, float bToUse, float aToUse = 1.f)
        : r(rToUse)
        , g(gToUse)
        , b(bToUse)
        , a(aToUse)
    {
    }

    static constexpr Color gray(float value, float alpha = 1.f)
    {
        return {value, value, value, alpha};
    }

    static constexpr Color white(float alpha = 1.f) { return {1.f, 1.f, 1.f, alpha}; }
    static constexpr Color black(float alpha = 1.f) { return {0.f, 0.f, 0.f, alpha}; }

    constexpr Color withAlpha(float alpha) const { return {r, g, b, alpha}; }

    constexpr Color brighter(float amount = 0.1f) const
    {
        return {std::min(r + amount, 1.f),
                std::min(g + amount, 1.f),
                std::min(b + amount, 1.f),
                a};
    }

    constexpr Color darker(float amount = 0.1f) const
    {
        return {std::max(r - amount, 0.f),
                std::max(g - amount, 0.f),
                std::max(b - amount, 0.f),
                a};
    }

    float r = 0.f;
    float g = 0.f;
    float b = 0.f;
    float a = 1.f;
};

struct GradientStop
{
    Color color;
    float position = 0.f;
};

struct LinearGradient
{
    LinearGradient() = default;
    LinearGradient(Point startToUse,
                   Point endToUse,
                   std::initializer_list<GradientStop> stopsToUse);

    Point start;
    Point end;
    Vector<GradientStop> stops;
};

} // namespace eacp::Graphics

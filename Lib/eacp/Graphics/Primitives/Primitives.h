#pragma once

#include <initializer_list>
#include <vector>

namespace eacp::Graphics
{
struct Point
{
    Point() = default;
    Point(float xToUse, float yToUse);

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

    float x = 0.f;
    float y = 0.f;
    float w = 0.f;
    float h = 0.f;
};

struct Color
{
    Color() = default;
    Color(float rToUse, float gToUse, float bToUse, float aToUse = 1.f);

    Color withAlpha(float alpha) const;

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
    LinearGradient(Point startToUse, Point endToUse,
                   std::initializer_list<GradientStop> stopsToUse);

    Point start;
    Point end;
    std::vector<GradientStop> stops;
};

} // namespace eacp::Graphics

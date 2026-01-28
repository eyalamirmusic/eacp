#pragma once

#include <initializer_list>
#include <vector>

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

struct Color
{
    Color() = default;
    Color(float rToUse, float gToUse, float bToUse, float aToUse = 1.f);

    static Color gray(float value, float alpha = 1.f);
    static Color white(float alpha = 1.f);
    static Color black(float alpha = 1.f);

    Color withAlpha(float alpha) const;
    Color brighter(float amount = 0.1f) const;
    Color darker(float amount = 0.1f) const;

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
    std::vector<GradientStop> stops;
};

} // namespace eacp::Graphics

#pragma once

namespace eacp::Graphics
{
struct Point
{
    Point() = default;
    Point(float xToUse, float yToUse);

    float x = 0.f;
    float y = 0.f;
};

struct Rect
{
    Rect() = default;
    Rect(float xToUse, float yToUse, float wToUse, float hToUse);

    float x = 0.f;
    float y = 0.f;
    float w = 0.f;
    float h = 0.f;
};

struct Color
{
    Color() = default;
    Color(float rToUse, float gToUse, float bToUse, float aToUse = 1.f);

    float r = 0.f;
    float g = 0.f;
    float b = 0.f;
    float a = 1.f;
};

}

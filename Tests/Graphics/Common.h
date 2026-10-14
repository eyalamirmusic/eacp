#pragma once

#include <eacp/Graphics/Graphics.h>
#include <NanoTest/NanoTest.h>

#include <cmath>

inline bool same(double a, double b)
{
    return std::abs(a - b) < 0.0001;
}

inline bool same(eacp::Graphics::Point point, eacp::Graphics::Point expected)
{
    return same(point.x, expected.x) && same(point.y, expected.y);
}

inline bool same(const eacp::Graphics::Rect& rect,
                 const eacp::Graphics::Rect& expected)
{
    return same(rect.x, expected.x) && same(rect.y, expected.y)
           && same(rect.w, expected.w) && same(rect.h, expected.h);
}

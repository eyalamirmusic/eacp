#pragma once

#include <algorithm>

namespace eacp::Graphics
{

constexpr float minimumMagnifyScale = 0.01f;

constexpr float magnifyScaleFromMagnification(double magnification)
{
    return std::max(minimumMagnifyScale, static_cast<float>(1.0 + magnification));
}

constexpr float magnifyScaleBetweenDistances(double previous, double current)
{
    if (previous <= 0.0 || current <= 0.0)
        return 1.f;

    return std::max(minimumMagnifyScale, static_cast<float>(current / previous));
}

} // namespace eacp::Graphics

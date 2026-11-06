#pragma once

/** @file Converting platform pinch gestures to MouseEvent::magnifyScale. */

#include <algorithm>

namespace eacp::Graphics
{

constexpr float minimumMagnifyScale = 0.01f;

// AppKit's additive magnification to a zoom factor, never below the minimum.
constexpr float magnifyScaleFromMagnification(double magnification)
{
    return std::max(minimumMagnifyScale, static_cast<float>(1.0 + magnification));
}

// Pinch zoom factor from two finger distances; 1 when either is not positive.
constexpr float magnifyScaleBetweenDistances(double previous, double current)
{
    if (previous <= 0.0 || current <= 0.0)
        return 1.f;

    return std::max(minimumMagnifyScale, static_cast<float>(current / previous));
}

} // namespace eacp::Graphics

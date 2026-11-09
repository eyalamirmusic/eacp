#include "Primitives.h"
#include <algorithm>

namespace eacp::Graphics
{
float clampedCornerRadius(const Rect& rect, float radius)
{
    auto largestFittingRadius = std::max(std::min(rect.w, rect.h) / 2.f, 0.f);
    return std::clamp(radius, 0.f, largestFittingRadius);
}

Color Color::brighter(float amount) const
{
    return {std::min(r + amount, 1.f),
            std::min(g + amount, 1.f),
            std::min(b + amount, 1.f),
            a};
}

Color Color::darker(float amount) const
{
    return {std::max(r - amount, 0.f),
            std::max(g - amount, 0.f),
            std::max(b - amount, 0.f),
            a};
}

LinearGradient::LinearGradient(Point startToUse,
                               Point endToUse,
                               std::initializer_list<GradientStop> stopsToUse)
    : start(startToUse)
    , end(endToUse)
    , stops(stopsToUse)
{
}

} // namespace eacp::Graphics
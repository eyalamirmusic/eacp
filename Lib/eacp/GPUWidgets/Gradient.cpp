#include "Gradient.h"

#include <algorithm>

namespace eacp::GPUWidgets
{
namespace
{
Graphics::Color
    lerpColor(const Graphics::Color& a, const Graphics::Color& b, float t)
{
    return {a.r + (b.r - a.r) * t,
            a.g + (b.g - a.g) * t,
            a.b + (b.b - a.b) * t,
            a.a + (b.a - a.a) * t};
}
} // namespace

Graphics::Color colorAt(const Graphics::LinearGradient& gradient,
                        const Graphics::Point& point)
{
    const auto& stops = gradient.stops;

    if (stops.empty())
        return Graphics::Color::white();

    if (stops.size() == 1)
        return stops[0].color;

    // Project the point onto the gradient axis to a parameter in [0, 1].
    auto axisX = gradient.end.x - gradient.start.x;
    auto axisY = gradient.end.y - gradient.start.y;
    auto lengthSquared = axisX * axisX + axisY * axisY;

    auto t = 0.0f;

    if (lengthSquared > 1e-12f)
    {
        auto dx = point.x - gradient.start.x;
        auto dy = point.y - gradient.start.y;
        t = (dx * axisX + dy * axisY) / lengthSquared;
    }

    t = std::clamp(t, 0.0f, 1.0f);

    auto sorted = stops;
    std::sort(sorted.begin(),
              sorted.end(),
              [](const Graphics::GradientStop& a, const Graphics::GradientStop& b)
              { return a.position < b.position; });

    if (t <= sorted.front().position)
        return sorted.front().color;

    if (t >= sorted.back().position)
        return sorted.back().color;

    for (auto i = 0; i + 1 < sorted.size(); ++i)
    {
        const auto& low = sorted[i];
        const auto& high = sorted[i + 1];

        if (t >= low.position && t <= high.position)
        {
            auto span = high.position - low.position;
            auto localT = span > 1e-12f ? (t - low.position) / span : 0.0f;
            return lerpColor(low.color, high.color, localT);
        }
    }

    return sorted.back().color;
}
} // namespace eacp::GPUWidgets

#pragma once

#include <eacp/GPUWidgets/GPUWidgets.h>

#include <algorithm>
#include <cmath>

// The shapes the batch tests mix, shared by the suites that rasterize them and
// the one that runs their binning stages on the CPU.
namespace eacp::GPUWidgets::shapes
{
inline Path star(Graphics::Rect bounds, int points)
{
    auto centre = bounds.center();
    auto radius = std::min(bounds.w, bounds.h) * 0.5f;

    auto path = Path {};

    // Every other vertex, which is what makes the outline cross itself and the
    // two fill rules disagree about the middle.
    for (auto i = 0; i < points; ++i)
    {
        auto angle = 2.f * pi * (float) (i * 2 % points) / (float) points;
        auto at = Graphics::Point {centre.x + std::sin(angle) * radius,
                                   centre.y - std::cos(angle) * radius};

        if (i == 0)
            path.moveTo(at);
        else
            path.lineTo(at);
    }

    path.close();
    return path;
}

inline Path ellipse(Graphics::Rect bounds)
{
    auto path = Path {};
    path.addEllipse(bounds);
    return path;
}

inline Path roundedRect(Graphics::Rect bounds, float radius)
{
    auto path = Path {};
    path.addRoundedRect(bounds, radius);
    return path;
}
} // namespace eacp::GPUWidgets::shapes

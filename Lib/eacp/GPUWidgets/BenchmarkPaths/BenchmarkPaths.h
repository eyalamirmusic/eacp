#pragma once

#include <eacp/GPUWidgets/GPUWidgets.h>

#include <algorithm>
#include <cmath>

namespace eacp::GPUWidgets::BenchmarkPaths
{
using Graphics::Point;
using Graphics::Rect;

inline Point onCircle(Point centre, float radius, float angle)
{
    return {centre.x + std::sin(angle) * radius,
            centre.y - std::cos(angle) * radius};
}

// A rotary indicator of the ring-and-pointer kind: a ring segment as a closed
// contour, and the pointer as a second one wound the same way. Two contours
// that overlap, which is what makes it worth timing.
inline GPUWidgets::Path knobIndicator(float size, float value)
{
    constexpr auto startAngle = -2.356194f;
    constexpr auto sweepAngle = 4.712389f;

    auto centre = Point {size * 0.5f, size * 0.5f};
    auto outer = size * 0.5f - 1.f;
    auto thickness = std::max(2.f, size * 0.12f);
    auto inner = outer - thickness;
    auto sweep = sweepAngle * value;
    auto steps = std::max(8, (int) std::ceil(sweep * outer * 0.5f));

    auto path = GPUWidgets::Path {};
    path.moveTo(onCircle(centre, outer, startAngle));

    for (auto i = 1; i <= steps; ++i)
        path.lineTo(
            onCircle(centre, outer, startAngle + sweep * (float) i / (float) steps));

    for (auto i = steps; i >= 0; --i)
        path.lineTo(
            onCircle(centre, inner, startAngle + sweep * (float) i / (float) steps));

    path.close();

    auto pointerWidth = std::max(1.5f, size * 0.045f);
    auto angle = startAngle + sweep;
    auto tip = onCircle(centre, outer - thickness * 0.5f, angle);
    auto across =
        Point {std::cos(angle) * pointerWidth, std::sin(angle) * pointerWidth};

    path.moveTo({centre.x - across.x, centre.y - across.y});
    path.lineTo({tip.x - across.x, tip.y - across.y});
    path.lineTo({tip.x + across.x, tip.y + across.y});
    path.lineTo({centre.x + across.x, centre.y + across.y});
    path.close();

    return path;
}

// Apps/GPU/PathQuality's panel: a rounded rectangle and an ellipse, which
// between them cover the straight edges and the near-tangent curvature a UI
// actually asks for.
inline GPUWidgets::Path qualityPanel(const Rect& panel)
{
    auto rounded =
        Rect {panel.w * 0.08f, panel.h * 0.05f, panel.w * 0.84f, panel.h * 0.30f};
    auto size = std::min(panel.w * 0.72f, panel.h * 0.48f);
    auto ellipse = Rect {(panel.w - size) * 0.5f, panel.h * 0.44f, size, size};

    auto path = GPUWidgets::Path {};
    path.addRoundedRect(rounded, rounded.h * 0.34f);
    path.addEllipse(ellipse);
    return path;
}

// The case the whole rasterizer is aimed at: a wide, shallow, curve-heavy region
// of the kind a DAW draws under an automation lane. Long in x, thin in y, and
// nearly all outline - the shape for which area-priced rasterization was
// hopeless and outline-priced rasterization is ordinary.
inline GPUWidgets::Path automationCurve(float width, float height, int lobes)
{
    auto path = GPUWidgets::Path {};
    path.moveTo({0.f, height * 0.5f});

    auto span = width / (float) lobes;

    for (auto i = 0; i < lobes; ++i)
    {
        auto x = span * (float) i;
        auto rising = (i % 2) == 0;
        auto to = rising ? height * 0.08f : height * 0.92f;
        auto from = rising ? height * 0.92f : height * 0.08f;

        path.cubicTo(x + span * 0.35f, from, x + span * 0.65f, to, x + span, to);
    }

    // Closed back along the lane's floor, so it is a filled region rather than a
    // ribbon - which is what an automation lane draws.
    path.lineTo({width, height});
    path.lineTo({0.f, height});
    path.close();

    return path;
}

// An outline with a real segment count behind it: the map layer, the SVG
// document, the vector editor's artwork. Concentric rings rather than one
// enormous contour, because that is the shape those actually have - many
// contours over one area, which is also what makes them overlap.
inline GPUWidgets::Path denseArtwork(float extent, int rings, int sides)
{
    auto path = GPUWidgets::Path {};
    auto centre = Point {extent * 0.5f, extent * 0.5f};

    for (auto ring = 0; ring < rings; ++ring)
    {
        auto radius = extent * 0.48f * (float) (ring + 1) / (float) rings;

        path.moveTo(onCircle(centre, radius, 0.f));

        for (auto i = 1; i < sides; ++i)
        {
            constexpr auto tau = 6.2831853f;
            auto angle = tau * (float) i / (float) sides;

            // A wobble on the radius, so no two rings are the same polygon and
            // no segment is exactly horizontal.
            auto wobble = 1.f + 0.06f * std::sin(angle * 7.f + (float) ring);
            path.lineTo(onCircle(centre, radius * wobble, angle));
        }

        path.close();
    }

    return path;
}

inline GPUWidgets::Path fullWindowEllipse(float width, float height)
{
    auto path = GPUWidgets::Path {};
    path.addEllipse({0.f, 0.f, width, height});
    return path;
}
} // namespace eacp::GPUWidgets::BenchmarkPaths

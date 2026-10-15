#pragma once

#include <eacp/Graphics/Graphics.h>
#include <eacp/Text/Text.h>

namespace eacp::UI
{
// Aliases rather than `using namespace eacp::Graphics`, because this module
// declares a class called Graphics: pulling the namespace in would make every
// later mention of `Graphics::` ambiguous at best and silently resolve to the
// painter at worst. Naming the three types the module actually spells a few
// hundred times leaves `Graphics` free to be the painter, which is the name a
// paint() signature wants.
using Color = eacp::Graphics::Color;
using Point = eacp::Graphics::Point;
using Rect = eacp::Graphics::Rect;
using GradientStop = eacp::Graphics::GradientStop;

// A face to draw a run of text in. Text::Font rather than Graphics::Font, which
// is the native tier's CoreText object: this one is a value naming a family, a
// size and a style, and what resolves it is the glyph atlas every component in
// the tree shares.
using Font = eacp::Text::Font;
using FontStyle = eacp::Text::FontStyle;

// The platform's stock UI face, the proportional sibling of
// Text::defaultMonospaceFamily. Same reasoning: no family name ships on all
// three systems, and asking for a literal one gets a substitute on the others.
constexpr const char* defaultUIFontFamily()
{
    if constexpr (Platform::isWindows())
        return "Segoe UI";

    if constexpr (Platform::isLinux())
        return "DejaVu Sans";

    if constexpr (Platform::isAndroid())
        return "sans-serif";

    return "Helvetica Neue";
}

// True when `outer` covers every point of `inner`. The clip logic asks this of
// every primitive drawn, and Rect offers contains(Point) only.
bool contains(const Rect& outer, const Rect& inner);

constexpr bool sameRect(const Rect& a, const Rect& b)
{
    return a.x == b.x && a.y == b.y && a.w == b.w && a.h == b.h;
}

// One radius per corner, clockwise from the top-left: the order CSS lists them
// in, and the reason there are four rather than one.
struct CornerRadii
{
    float topLeft = 0.f;
    float topRight = 0.f;
    float bottomRight = 0.f;
    float bottomLeft = 0.f;

    static constexpr CornerRadii all(float radius)
    {
        return {radius, radius, radius, radius};
    }
};

// A rectangle whose corners are rounded, each by its own radius. A draw that
// takes one cuts by it only where it is rounded: an empty rect, or one whose
// radii are all zero, is uncut -- a square cut being the clip's job.
struct RoundedRect
{
    Rect rect;
    CornerRadii radii;

    constexpr bool isEmpty() const { return rect.w <= 0.f || rect.h <= 0.f; }

    constexpr bool isRounded() const
    {
        return !isEmpty()
               && (radii.topLeft > 0.f || radii.topRight > 0.f
                   || radii.bottomRight > 0.f || radii.bottomLeft > 0.f);
    }
};

// The radii the rect can actually hold. Where two corners on one side ask for
// more than the side is long, all four shrink by the same factor -- CSS
// Backgrounds 3 5.5 -- so the shape keeps its proportions rather than folding
// through itself. One radius for all four is clamped to half the shorter side,
// which is what fillRoundedRect does.
CornerRadii fittedRadii(const RoundedRect& shape);
} // namespace eacp::UI

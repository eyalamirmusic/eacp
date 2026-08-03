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
// Text::defaultMonospaceFamily. Same reasoning: no family name ships on both
// systems, and asking for a literal one gets a substitute on the other.
constexpr const char* defaultUIFontFamily()
{
#if defined(_WIN32)
    return "Segoe UI";
#else
    return "Helvetica Neue";
#endif
}

// True when `outer` covers every point of `inner`. The clip logic asks this of
// every primitive drawn, and Rect offers contains(Point) only.
inline bool contains(const Rect& outer, const Rect& inner)
{
    if (inner.isEmpty())
        return true;

    return inner.left() >= outer.left() && inner.right() <= outer.right()
           && inner.top() >= outer.top() && inner.bottom() <= outer.bottom();
}

inline bool sameRect(const Rect& a, const Rect& b)
{
    return a.x == b.x && a.y == b.y && a.w == b.w && a.h == b.h;
}
} // namespace eacp::UI

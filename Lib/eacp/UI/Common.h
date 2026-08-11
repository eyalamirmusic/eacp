#pragma once

#include <eacp/Graphics/Graphics.h>
#include <eacp/Text/Text.h>

namespace eacp::UI
{
// Aliases, not `using namespace eacp::Graphics`: this module's own Graphics
// class must keep the unqualified name.
using Color = eacp::Graphics::Color;
using Point = eacp::Graphics::Point;
using Rect = eacp::Graphics::Rect;
using GradientStop = eacp::Graphics::GradientStop;

// A value naming family/size/style, resolved by the shared glyph atlas - not
// the native tier's Graphics::Font.
using Font = eacp::Text::Font;
using FontStyle = eacp::Text::FontStyle;

// The platform's stock UI face: no family name ships on both systems.
constexpr const char* defaultUIFontFamily()
{
#if defined(_WIN32)
    return "Segoe UI";
#else
    return "Helvetica Neue";
#endif
}

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

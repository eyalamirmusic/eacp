#include "WindowGeometry-Windows.h"

#include <algorithm>
#include <cmath>

namespace eacp::Graphics::detail
{
RECT toPhysicalPixels(const Rect& bounds, float scale)
{
    auto left = std::floor(bounds.x * scale);
    auto top = std::floor(bounds.y * scale);
    auto right = std::ceil((bounds.x + bounds.w) * scale);
    auto bottom = std::ceil((bounds.y + bounds.h) * scale);

    return {static_cast<LONG>(left),
            static_cast<LONG>(top),
            static_cast<LONG>(right),
            static_cast<LONG>(bottom)};
}

void containWithinWorkArea(RECT& frame, const RECT& work)
{
    auto maxWidth = work.right - work.left;
    auto maxHeight = work.bottom - work.top;
    auto width = std::min(frame.right - frame.left, maxWidth);
    auto height = std::min(frame.bottom - frame.top, maxHeight);

    auto left = std::clamp(frame.left, work.left, work.right - width);
    auto top = std::clamp(frame.top, work.top, work.bottom - height);

    frame = {left, top, left + width, top + height};
}

LRESULT resizeBandHitTest(const RECT& frame, POINT point, LONG band)
{
    auto onLeft = point.x < frame.left + band;
    auto onRight = point.x >= frame.right - band;
    auto onTop = point.y < frame.top + band;
    auto onBottom = point.y >= frame.bottom - band;

    if (!onLeft && !onRight && !onTop && !onBottom)
        return HTCLIENT;

    auto corner = band * 2;
    auto nearLeft = point.x < frame.left + corner;
    auto nearRight = point.x >= frame.right - corner;
    auto nearTop = point.y < frame.top + corner;
    auto nearBottom = point.y >= frame.bottom - corner;

    if ((onTop && nearLeft) || (onLeft && nearTop))
        return HTTOPLEFT;
    if ((onTop && nearRight) || (onRight && nearTop))
        return HTTOPRIGHT;
    if ((onBottom && nearLeft) || (onLeft && nearBottom))
        return HTBOTTOMLEFT;
    if ((onBottom && nearRight) || (onRight && nearBottom))
        return HTBOTTOMRIGHT;

    if (onLeft)
        return HTLEFT;
    if (onRight)
        return HTRIGHT;
    if (onTop)
        return HTTOP;
    return HTBOTTOM;
}
} // namespace eacp::Graphics::detail

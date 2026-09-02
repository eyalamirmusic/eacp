#pragma once

#include <eacp/Core/Utils/WinInclude.h>

#include "../Primitives/Primitives.h"

#include <algorithm>
#include <cmath>

// The geometry behind the placement rules a Win32 surface has to enforce for
// itself, kept apart from the HWND that feeds them so all of it can be checked
// without a display: where a point lands on a window's own resize band, how a
// window too big for its display is brought back within reach, and which
// pixels a rect measured in points covers.
namespace eacp::Graphics::detail
{
// The pixels a rect of ours covers, for placing and sizing a window in the
// physical units Win32 measures one in. `scale` is pixels per point.
//
// Grown to the smallest pixel rect containing the points, rather than rounded
// to the nearest. A surface rounded down leaves a seam of whatever is behind
// it showing along its right and bottom edges, and against a host's own window
// that seam is visible in a way half a pixel of overlap is not.
inline RECT toPhysicalPixels(const Rect& bounds, float scale)
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

// Shrinks and slides a window rect until the whole of it lies inside `work`
// (the display's work area — the monitor minus the taskbar and any appbars).
//
// A window is asked for in points and opens in pixels, so a size that is
// comfortable on the display it was chosen on is routinely bigger than a 200%
// laptop's whole screen. What then hangs off the bottom right is the resize
// corner, which is also the way back — so the one window the user cannot fix
// by dragging is the one that most needs it.
//
// keepAspectRatio trims both sides by the same factor: shrinking them
// independently would hand a ratio-locked window the one shape it exists to
// refuse.
inline void
    containWithinWorkArea(RECT& frame, const RECT& work, bool keepAspectRatio)
{
    auto width = frame.right - frame.left;
    auto height = frame.bottom - frame.top;
    auto maxWidth = work.right - work.left;
    auto maxHeight = work.bottom - work.top;

    if (keepAspectRatio && width > 0 && height > 0
        && (width > maxWidth || height > maxHeight))
    {
        auto factor = std::min(static_cast<double>(maxWidth) / width,
                               static_cast<double>(maxHeight) / height);
        width = static_cast<LONG>(width * factor);
        height = static_cast<LONG>(height * factor);
    }

    width = std::min(width, maxWidth);
    height = std::min(height, maxHeight);

    auto left = std::clamp(frame.left, work.left, work.right - width);
    auto top = std::clamp(frame.top, work.top, work.bottom - height);

    frame = {left, top, left + width, top + height};
}

// Where `point` (screen coordinates) lands on a window whose client area is
// its whole rect, as an HT* hit-test code.
//
// A frameless window eats its frame in WM_NCCALCSIZE, and DefWindowProc
// answers HTCLIENT for every point inside the client rect — so eating the
// frame also ate the border band it hit-tests for resizing, and no edge of the
// window can be dragged, WS_THICKFRAME or not. This measures out the band the
// frame would have reserved.
//
// `band` is the edge thickness in physical pixels. A corner reaches twice that
// far along both of its edges, the way a titled window's does: a band-square
// diagonal grab is a target the user has to aim at.
inline LRESULT resizeBandHitTest(const RECT& frame, POINT point, LONG band)
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

#pragma once

#include "../Primitives/Primitives.h"

namespace eacp::Graphics
{
// Rects are in points, like WindowOptions::width/height, with the origin at the
// primary display's top-left growing right and down.
struct Display
{
    // The whole display, menu bar / taskbar included.
    Rect frame;

    // The part an ordinary window can occupy: frame minus the menu bar, Dock,
    // taskbar and appbars.
    Rect workArea;

    // Pixels per point: 2 on Retina, 1.5 at Windows' 150%.
    float backingScale = 1.f;
};

// Under headless, or when the platform reports nothing, comes back as a
// plausible 1280x800 at scale 1 rather than zeroes.
Display primaryDisplay();
} // namespace eacp::Graphics

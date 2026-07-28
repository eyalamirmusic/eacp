#pragma once

#include "../Primitives/Primitives.h"

namespace eacp::Graphics
{
// What a display is, from an app's point of view - which is: how big a window
// may I make, and where may I put it.
//
// Everything here is in points, the same unit WindowOptions::width/height and
// Window::setBounds are in, so a size read here can be handed straight to a
// window with no conversion. Points, not pixels, is the whole reason this is
// worth a type: an app that reasoned in pixels would open a window twice the
// intended size on a Retina display and a quarter of it on a 4K panel at 150%.
//
// The origin is the primary display's top-left, growing right and down, which
// is what WindowOptions::initialPosition already means by a screen point.
struct Display
{
    // The whole display, menu bar / taskbar included.
    Rect frame;

    // The part an ordinary window can occupy: frame minus the menu bar and the
    // Dock on macOS, minus the taskbar and any appbars on Windows. This is what
    // an initial size should be fitted into and what a window should be clamped
    // to - the frame is bigger than anything a window may usefully be.
    Rect workArea;

    // Pixels per point on this display: 2 on a Retina panel, 1.5 at Windows'
    // 150%. Only needed by an app sizing itself against a *pixel* grid - a
    // renderer with a native resolution wanting an integer scale factor. Window
    // sizing itself needs none of it, points being points.
    float backingScale = 1.f;
};

// The display an app should size its first window against - the one carrying
// the menu bar on macOS, the one Windows calls primary. Under headless, and if
// the platform reports nothing, it comes back as a plausible 1280x800 at scale
// 1 rather than zeroes, so an app dividing by it does not have to guard.
Display primaryDisplay();
} // namespace eacp::Graphics

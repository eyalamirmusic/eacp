#include "Display.h"

namespace eacp::Graphics
{
// The fallback Display.h already documents, as the only answer there is: no
// Wayland connection means no wl_output, and therefore no mode, no geometry and
// no scale to report. 1280x800 at scale 1 is a plausible size for an app to
// place its first window against, which is what this type is for, and it is the
// same shape Display-iOS.mm falls back to when there is no screen.
//
// Stage 4 replaces it with the wl_output the surface is actually on: geometry
// and mode give the frame, xdg_output the logical size, and
// wp_fractional_scale the backing scale. There is no work area in Wayland —
// panels are ordinary clients — so workArea stays equal to frame there too
// unless a desktop portal offers one.
Display primaryDisplay()
{
    const auto frame = Rect {0.f, 0.f, 1280.f, 800.f};

    return {frame, frame, 1.f};
}
} // namespace eacp::Graphics

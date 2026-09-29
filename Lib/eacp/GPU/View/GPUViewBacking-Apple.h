#pragma once

// Internal seam between the shared Apple GPUView (GPUView-Apple.mm) and the
// per-platform piece. The CAMetalLayer drawable size is logical points times the
// backing scale, which macOS and iOS read from different places.

namespace eacp::GPU
{
class GPUView;

// The view's backing scale (device pixels per logical point). Read from the
// window/screen on macOS (NSWindow/NSScreen), from the UIView on iOS. Defined in
// GPUView-macOS.mm / GPUView-iOS.mm.
double platformBackingScale(GPUView& view);

// Whether the window this view is in composites as opaque, which is what the
// CAMetalLayer's own opaque flag has to agree with: a layer left opaque in a
// WindowOptions::transparentBackground window has its alpha dropped, so a
// shader writing a < 1 shows the window's cleared black instead of the desktop.
// True with no window yet, and always on iOS, which has no transparent window.
bool platformWindowIsOpaque(GPUView& view);
} // namespace eacp::GPU

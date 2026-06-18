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
} // namespace eacp::GPU

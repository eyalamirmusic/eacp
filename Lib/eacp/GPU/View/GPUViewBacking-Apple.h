#pragma once

// Internal seam between the shared Apple GPUView (GPUView-Apple.mm) and the
// per-platform piece. The CAMetalLayer drawable size is logical points times the
// backing scale, which macOS and iOS read from different places.

#include "ContinuousRenderSwitch.h"

namespace eacp::GPU
{
class GPUView;

ViewVisibility platformViewVisibility(GPUView& view);

bool platformIsInLiveResize(GPUView& view);

// Calls `changed` on the main thread whenever any window's occlusion state
// changes. The returned token goes to platformStopObservingWindowVisibility.
void* platformObserveWindowVisibility(const Callback& changed);
void platformStopObservingWindowVisibility(void* token);

// The view's backing scale (device pixels per logical point). Read from the
// window/screen on macOS (NSWindow/NSScreen), from the UIView on iOS. Defined in
// GPUView-macOS.mm / GPUView-iOS.mm.
double platformBackingScale(GPUView& view);
} // namespace eacp::GPU

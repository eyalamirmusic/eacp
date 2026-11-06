#pragma once

/** @file
    Internal: the per-platform half of the Apple GPUView (scale, visibility, live
    resize).
*/

// Internal seam between the shared Apple GPUView (GPUView-Apple.mm) and the
// per-platform piece. The CAMetalLayer drawable size is logical points times the
// backing scale, which macOS and iOS read from different places.

#include <eacp/Core/Utils/Common.h>

namespace eacp::GPU
{
class GPUView;

enum class ViewVisibility
{
    Visible,
    WindowNotVisible,
    NotInVisibleWindow
};

/// Visible: drawn on screen. WindowNotVisible: the view's window is minimised,
/// ordered out, covered or on a sleeping display, and says so when that ends.
/// NotInVisibleWindow: no window, or the view is hidden, so no notification
/// will announce the view coming back.
ViewVisibility platformViewVisibility(GPUView& view);

bool platformIsInLiveResize(GPUView& view);

/// Calls `changed` on the main thread whenever any window's occlusion state
/// changes. The returned token goes to platformStopObservingWindowVisibility.
void* platformObserveWindowVisibility(const Callback& changed);
void platformStopObservingWindowVisibility(void* token);

/// The view's backing scale (device pixels per logical point). Read from the
/// window/screen on macOS (NSWindow/NSScreen), from the UIView on iOS. Defined in
/// GPUView-macOS.mm / GPUView-iOS.mm.
double platformBackingScale(GPUView& view);
} // namespace eacp::GPU

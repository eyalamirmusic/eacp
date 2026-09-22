#pragma once

#include "../Primitives/Primitives.h"

#include <CoreGraphics/CGGeometry.h>

@class NSWindow;

namespace eacp::Graphics
{
// Whether this window is a WindowOptions::popup one — a menu, a dropdown, a
// tooltip that never takes key focus.
//
// A view inside one has to track the mouse with NSTrackingActiveAlways, since
// the window it is in is deliberately never key and an ActiveInKeyWindow area
// would never fire: a menu whose items do not highlight under the pointer.
bool isPopupWindow(NSWindow* window);

// AppKit measures screen points from the primary screen's bottom-left with y
// growing up; eacp measures them from its top-left with y growing down (see
// Display, and WindowOptions::initialPosition). The flip, in one place, both
// ways round.
Point screenPointFromAppKit(CGPoint appKitPoint);
CGPoint appKitPointFromScreen(Point screenPoint);
} // namespace eacp::Graphics

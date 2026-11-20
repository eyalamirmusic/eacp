#pragma once

@class NSWindow;

namespace eacp::Graphics
{
bool isFrameworkWindow(NSWindow* window);

// The primary screen's top edge in AppKit screen points: the line every
// AppKit <-> eacp screen point conversion flips about.
double primaryScreenTop();
} // namespace eacp::Graphics

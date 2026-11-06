#pragma once

/** @file Internal: macOS only. Asks whether an NSWindow is one eacp created. */

@class NSWindow;

namespace eacp::Graphics
{
bool isFrameworkWindow(NSWindow* window);
} // namespace eacp::Graphics

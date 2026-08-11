#pragma once

namespace eacp::GPU
{
class GPUView;

// Device pixels per logical point. Defined in GPUView-macOS.mm / GPUView-iOS.mm,
// which read it from NSWindow/NSScreen and from the UIView respectively.
double platformBackingScale(GPUView& view);
} // namespace eacp::GPU

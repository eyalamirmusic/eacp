#pragma once

#include <eacp/Graphics/Primitives/Primitives.h>

#include <cstdint>

namespace eacp::GPU
{
struct ScissorPixels
{
    int left = 0;
    int top = 0;
    int right = 0;
    int bottom = 0;
};

bool hasTargetArea(int targetWidth, int targetHeight);

// Rounds outward before clamping to the target: rounding a scrolled region's
// edge inward would shave a column of glyph coverage off the boundary.
ScissorPixels calculateScissorPixels(const Graphics::Rect& rect,
                                     int targetWidth,
                                     int targetHeight);

// A viewport is a float rectangle and is not rounded; one with no area or
// reaching outside the target is refused rather than clipped.
bool isViewportInsideTarget(const Graphics::Rect& rect,
                            int targetWidth,
                            int targetHeight);

bool isOffsetInsideBuffer(std::int64_t offset, std::uint64_t bufferSize);
} // namespace eacp::GPU

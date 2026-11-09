#pragma once

#include <eacp/Graphics/Primitives/Primitives.h>

#include <cstdint>

namespace eacp::GPU
{
Graphics::Rect makeTargetRectangle(int targetWidth, int targetHeight);

// Rounds outward after clamping to the target: rounding a scrolled region's edge
// inward would shave a column of glyph coverage off the boundary.
Geometry::Rectangle<int> calculateScissorRectangle(const Graphics::Rect& rect,
                                                   const Graphics::Rect& target);

bool isOffsetInsideBuffer(std::int64_t offset, std::uint64_t bufferSize);
} // namespace eacp::GPU

#include "RenderPassRegions.h"

#include <algorithm>

namespace eacp::GPU
{
Graphics::Rect makeTargetRectangle(int targetWidth, int targetHeight)
{
    return {
        0.f, 0.f, static_cast<float>(targetWidth), static_cast<float>(targetHeight)};
}

// A region wholly past the right or bottom edge intersects to an empty rect that
// still starts past it, which Metal's validation refuses; it is pulled onto the
// edge.
Geometry::Rectangle<int> calculateScissorRectangle(const Graphics::Rect& rect,
                                                   const Graphics::Rect& target)
{
    const auto overlap = rect.intersection(target);
    const auto pinnedX = std::min(overlap.x, target.right());
    const auto pinnedY = std::min(overlap.y, target.bottom());
    return overlap.withPosition(pinnedX, pinnedY).roundedOutwardTo<int>();
}

bool isOffsetInsideBuffer(std::int64_t offset, std::uint64_t bufferSize)
{
    return offset >= 0 && static_cast<std::uint64_t>(offset) < bufferSize;
}
} // namespace eacp::GPU

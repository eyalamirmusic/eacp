#include "RenderPassRegions.h"

#include <algorithm>
#include <cmath>

namespace eacp::GPU
{
bool hasTargetArea(int targetWidth, int targetHeight)
{
    return targetWidth > 0 && targetHeight > 0;
}

ScissorPixels calculateScissorPixels(const Graphics::Rect& rect,
                                     int targetWidth,
                                     int targetHeight)
{
    const auto floorLeft = static_cast<int>(std::floor(rect.x));
    const auto floorTop = static_cast<int>(std::floor(rect.y));
    const auto ceilRight = static_cast<int>(std::ceil(rect.x + rect.w));
    const auto ceilBottom = static_cast<int>(std::ceil(rect.y + rect.h));

    auto pixels = ScissorPixels {};
    pixels.left = std::clamp(floorLeft, 0, targetWidth);
    pixels.top = std::clamp(floorTop, 0, targetHeight);
    pixels.right = std::clamp(ceilRight, pixels.left, targetWidth);
    pixels.bottom = std::clamp(ceilBottom, pixels.top, targetHeight);
    return pixels;
}

static bool hasNoViewportArea(const Graphics::Rect& rect)
{
    return rect.w <= 0.f || rect.h <= 0.f;
}

static bool startsBeforeViewportTarget(const Graphics::Rect& rect)
{
    return rect.x < 0.f || rect.y < 0.f;
}

static bool endsPastViewportTarget(const Graphics::Rect& rect,
                                   int targetWidth,
                                   int targetHeight)
{
    const auto width = static_cast<float>(targetWidth);
    const auto height = static_cast<float>(targetHeight);
    return rect.x + rect.w > width || rect.y + rect.h > height;
}

// Written as the negation of the refusals so a NaN edge passes, as it always has.
bool isViewportInsideTarget(const Graphics::Rect& rect,
                            int targetWidth,
                            int targetHeight)
{
    return !hasNoViewportArea(rect) && !startsBeforeViewportTarget(rect)
           && !endsPastViewportTarget(rect, targetWidth, targetHeight);
}

bool isOffsetInsideBuffer(std::int64_t offset, std::uint64_t bufferSize)
{
    return offset >= 0 && static_cast<std::uint64_t>(offset) < bufferSize;
}
} // namespace eacp::GPU

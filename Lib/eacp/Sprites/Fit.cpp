#include "Fit.h"

namespace eacp::Sprites
{
Rect fitRect(
    float areaWidth, float areaHeight, int imageWidth, int imageHeight, Fit fit)
{
    if (fit == Fit::Stretch || imageWidth <= 0 || imageHeight <= 0
        || areaWidth <= 0.0f || areaHeight <= 0.0f)
        return {0.0f, 0.0f, areaWidth, areaHeight};

    auto imageAspect = (float) imageWidth / (float) imageHeight;
    auto areaAspect = areaWidth / areaHeight;
    auto imageWider = imageAspect > areaAspect;

    auto widthLimited = fit == Fit::Contain ? imageWider : !imageWider;

    auto width = widthLimited ? areaWidth : areaHeight * imageAspect;
    auto height = widthLimited ? areaWidth / imageAspect : areaHeight;

    auto x = (areaWidth - width) * 0.5f;
    auto y = (areaHeight - height) * 0.5f;

    return {x, y, width, height};
}
} // namespace eacp::Sprites

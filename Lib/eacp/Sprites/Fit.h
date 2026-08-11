#pragma once

#include <eacp/GPU/GPU.h>

namespace eacp::Sprites
{
using Graphics::Rect;

enum class Fit
{
    Stretch, // fill the area, ignoring aspect (may distort)
    Contain, // fit entirely inside, letterboxing the remainder
    Cover // fill the area, cropping the overflow
};

Rect fitRect(
    float areaWidth, float areaHeight, int imageWidth, int imageHeight, Fit fit);
} // namespace eacp::Sprites

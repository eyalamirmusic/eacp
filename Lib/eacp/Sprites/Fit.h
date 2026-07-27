#pragma once

#include <eacp/GPU/GPU.h>

namespace eacp::Sprites
{
using Graphics::Rect;

// How an image is placed when its aspect ratio differs from the area it is
// drawn into.
enum class Fit
{
    Stretch, // fill the area, ignoring aspect (may distort)
    Contain, // fit entirely inside, letterboxing the remainder
    Cover // fill the area, cropping the overflow
};

// The destination rect for an imageWidth x imageHeight image inside an
// areaWidth x areaHeight area under the given fit. Pure geometry, shared by
// every view that puts a video-shaped texture on screen (CameraView,
// VideoView), which is why it lives here rather than on either of them.
Rect fitRect(
    float areaWidth, float areaHeight, int imageWidth, int imageHeight, Fit fit);
} // namespace eacp::Sprites

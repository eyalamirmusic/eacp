#pragma once

/** @file Placing an image in an area of a different aspect ratio. */

#include <eacp/Graphics/Primitives/Primitives.h>

namespace eacp::Sprites
{
using Graphics::Rect;

// How an image is placed when its aspect ratio differs from the area it is
// drawn into.
enum class Fit
{
    Stretch, // Fill the area, ignoring aspect; may distort.
    Contain, // Fit entirely inside, leaving bars on the remainder.
    Cover // Fill the area, cropping the overflow.
};

// The destination rect for an imageWidth x imageHeight image inside an
// areaWidth x areaHeight area under the given fit. Pure geometry, shared by
// every view that puts a video-shaped texture on screen (CameraView,
// VideoView), which is why it lives here rather than on either of them.
Rect fitRect(
    float areaWidth, float areaHeight, int imageWidth, int imageHeight, Fit fit);
} // namespace eacp::Sprites

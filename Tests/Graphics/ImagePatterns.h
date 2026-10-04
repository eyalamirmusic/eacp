#pragma once

#include "Common.h"

namespace ImagePatterns
{
using eacp::Graphics::Image;
using eacp::Graphics::ImageData;

// Deterministic, fully opaque RGBA pattern. Opaque alpha keeps the PNG
// round-trip byte-exact (no premultiplied-alpha precision loss).
inline Image makeOpaquePattern(int width, int height)
{
    auto rgba = ImageData {};
    rgba.reserve(width * height * 4);
    for (auto y = 0; y < height; ++y)
    {
        for (auto x = 0; x < width; ++x)
        {
            rgba.add(static_cast<std::uint8_t>(x * 7 + 1));
            rgba.add(static_cast<std::uint8_t>(y * 11 + 2));
            rgba.add(static_cast<std::uint8_t>((x + y) * 5 + 3));
            rgba.add(static_cast<std::uint8_t>(255));
        }
    }
    return Image(width, height, std::move(rgba));
}

// Deterministic pattern with varying (non-opaque) alpha. Exercises the
// straight-alpha decode path, which must not quantize through a
// premultiplied bitmap context.
inline Image makeTranslucentPattern(int width, int height)
{
    auto rgba = ImageData {};
    rgba.reserve(width * height * 4);
    for (auto y = 0; y < height; ++y)
    {
        for (auto x = 0; x < width; ++x)
        {
            rgba.add(static_cast<std::uint8_t>(200 - x * 9));
            rgba.add(static_cast<std::uint8_t>(40 + y * 13));
            rgba.add(static_cast<std::uint8_t>(50 + (x + y) * 6));
            rgba.add(static_cast<std::uint8_t>(16 + x * 17 + y * 3));
        }
    }
    return Image(width, height, std::move(rgba));
}
} // namespace ImagePatterns

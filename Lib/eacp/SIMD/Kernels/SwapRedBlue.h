#pragma once

#include "../Backend/Scalar.h"

namespace eacp::simd::kernels
{

// Swap the red and blue channels of `pixelCount` tightly-packed 8-bit pixels
// (RGBA <-> BGRA), leaving green and alpha in place. Written once over a
// backend B; the main loop processes B::U32::lanes pixels at a time and the
// remainder is finished through the Scalar backend so it stays bit-identical.
//
// On the little-endian targets eacp builds for, a [R,G,B,A] pixel reads as the
// u32 R | G<<8 | B<<16 | A<<24, so the swap is: red (bits 0..7) -> bits 16..23,
// blue (bits 16..23) -> bits 0..7, green/alpha untouched.
template <class B>
void swapRedBlueImpl(const std::uint8_t* in,
                     std::uint8_t* out,
                     std::size_t pixelCount)
{
    using V = typename B::U32;
    const auto redMask = V::broadcast(0x000000FFu);
    const auto blueMask = V::broadcast(0x00FF0000u);
    const auto greenAlphaMask = V::broadcast(0xFF00FF00u);

    std::size_t i = 0;
    for (; i + V::lanes <= pixelCount; i += V::lanes)
    {
        const auto pixels = V::load(in + i * 4);
        const auto red = (pixels & redMask).template shl<16>();
        const auto blue = (pixels & blueMask).template shr<16>();
        const auto greenAlpha = pixels & greenAlphaMask;
        V::store(out + i * 4, red | blue | greenAlpha);
    }

    if constexpr (V::lanes > 1)
        if (i < pixelCount)
            swapRedBlueImpl<backend::Scalar>(
                in + i * 4, out + i * 4, pixelCount - i);
}

} // namespace eacp::simd::kernels

#pragma once

#include <cmath>
#include <cstddef>
#include <cstdint>

namespace eacp::simd::kernels
{

inline int clampi(int v, int lo, int hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

// Bilinear resize of a tightly-packed RGBA8 image, written once over a backend B
// and run on every platform -- instantiating it with the Scalar backend makes
// it the reference oracle. Each output pixel's four channels are blended as one
// B::F4 in a fixed per-channel reduction order with the same weights, so all
// backends agree bit-for-bit when compiled with -ffp-contract=off (no silent
// multiply-add fusion). Half-pixel-center mapping with edge clamping (OpenCV
// semantics). No heap allocation: the source mapping is computed inline.
template <class B>
void resizeBilinearImpl(const std::uint8_t* in,
                        int srcW,
                        int srcH,
                        std::uint8_t* out,
                        int dstW,
                        int dstH)
{
    using F4 = typename B::F4;

    const float scaleX = static_cast<float>(srcW) / static_cast<float>(dstW);
    const float scaleY = static_cast<float>(srcH) / static_cast<float>(dstH);
    const int maxX = srcW - 1;
    const int maxY = srcH - 1;

    for (int dy = 0; dy < dstH; ++dy)
    {
        const float fy = (static_cast<float>(dy) + 0.5f) * scaleY - 0.5f;
        const int y0 = static_cast<int>(std::floor(fy));
        const float wy = fy - static_cast<float>(y0);
        const float oneMinusWy = 1.f - wy;
        const std::uint8_t* rowTop =
            in + static_cast<std::ptrdiff_t>(clampi(y0, 0, maxY)) * srcW * 4;
        const std::uint8_t* rowBot =
            in + static_cast<std::ptrdiff_t>(clampi(y0 + 1, 0, maxY)) * srcW * 4;

        for (int dx = 0; dx < dstW; ++dx)
        {
            const float fx = (static_cast<float>(dx) + 0.5f) * scaleX - 0.5f;
            const int x0 = static_cast<int>(std::floor(fx));
            const float wx = fx - static_cast<float>(x0);
            const float oneMinusWx = 1.f - wx;
            const int x0c = clampi(x0, 0, maxX);
            const int x1c = clampi(x0 + 1, 0, maxX);

            const float w00 = oneMinusWx * oneMinusWy;
            const float w10 = wx * oneMinusWy;
            const float w01 = oneMinusWx * wy;
            const float w11 = wx * wy;

            const std::uint8_t* p00 = rowTop + x0c * 4;
            const std::uint8_t* p10 = rowTop + x1c * 4;
            const std::uint8_t* p01 = rowBot + x0c * 4;
            const std::uint8_t* p11 = rowBot + x1c * 4;

            const auto acc = F4::broadcast(w00) * F4::loadPixel(p00)
                             + F4::broadcast(w10) * F4::loadPixel(p10)
                             + F4::broadcast(w01) * F4::loadPixel(p01)
                             + F4::broadcast(w11) * F4::loadPixel(p11);
            F4::storePixel(out, acc);
            out += 4;
        }
    }
}

} // namespace eacp::simd::kernels

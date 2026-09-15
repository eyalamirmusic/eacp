#pragma once

#include "Bilinear.h"

namespace eacp::simd::kernels
{

// Bilinear resize of a tightly-packed RGBA8 image, written once over a backend B
// and run on every platform -- instantiating it with the Scalar backend makes it
// the reference oracle. Half-pixel-center mapping with edge clamping (OpenCV
// semantics); no heap allocation (the source mapping is computed inline). The
// per-pixel blend is the shared blendTaps primitive, so all backends agree
// bit-for-bit when the TU is compiled -ffp-contract=off.
template <class B>
void resizeBilinearImpl(const std::uint8_t* in,
                        int srcW,
                        int srcH,
                        std::uint8_t* out,
                        int dstW,
                        int dstH)
{
    const auto scaleX = static_cast<float>(srcW) / static_cast<float>(dstW);
    const auto scaleY = static_cast<float>(srcH) / static_cast<float>(dstH);
    const auto maxX = srcW - 1;
    const auto maxY = srcH - 1;

    for (auto dy = 0; dy < dstH; ++dy)
    {
        const auto fy = (static_cast<float>(dy) + 0.5f) * scaleY - 0.5f;
        const auto y0 = static_cast<int>(std::floor(fy));
        const auto wy = fy - static_cast<float>(y0);
        const auto oneMinusWy = 1.f - wy;
        const auto* rowTop =
            in + static_cast<std::ptrdiff_t>(clampi(y0, 0, maxY)) * srcW * 4;
        const auto* rowBot =
            in + static_cast<std::ptrdiff_t>(clampi(y0 + 1, 0, maxY)) * srcW * 4;

        for (auto dx = 0; dx < dstW; ++dx)
        {
            const auto fx = (static_cast<float>(dx) + 0.5f) * scaleX - 0.5f;
            const auto x0 = static_cast<int>(std::floor(fx));
            const auto wx = fx - static_cast<float>(x0);
            const auto oneMinusWx = 1.f - wx;
            const auto x0c = clampi(x0, 0, maxX);
            const auto x1c = clampi(x0 + 1, 0, maxX);

            blendTaps<B>(rowTop + x0c * 4,
                         rowTop + x1c * 4,
                         rowBot + x0c * 4,
                         rowBot + x1c * 4,
                         oneMinusWx * oneMinusWy,
                         wx * oneMinusWy,
                         oneMinusWx * wy,
                         wx * wy,
                         out);
            out += 4;
        }
    }
}

} // namespace eacp::simd::kernels

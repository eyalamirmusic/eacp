#pragma once

#include "Bilinear.h"

namespace eacp::simd::kernels
{

// Inverse affine warp of a tightly-packed RGBA8 image: for each destination
// pixel, map it back through the 2x3 row-major inverse matrix `m`
// ([m0 m1 m2; m3 m4 m5]) to a source position, then bilinearly sample with edge
// clamping. Same structure (and the same shared blendTaps blend) as
// resizeBilinear -- only the per-pixel source coordinate differs (affine instead
// of a regular grid). Instantiating with the Scalar backend is the oracle;
// bit-exact across backends when the TU is -ffp-contract=off. No heap allocation.
template <class B>
void warpAffineInverseImpl(const std::uint8_t* in,
                           int srcW,
                           int srcH,
                           const float* m,
                           std::uint8_t* out,
                           int dstW,
                           int dstH)
{
    const auto maxX = srcW - 1;
    const auto maxY = srcH - 1;

    for (auto dy = 0; dy < dstH; ++dy)
    {
        const auto fdy = static_cast<float>(dy);

        for (auto dx = 0; dx < dstW; ++dx)
        {
            const auto fdx = static_cast<float>(dx);
            const auto srcX = m[0] * fdx + m[1] * fdy + m[2];
            const auto srcY = m[3] * fdx + m[4] * fdy + m[5];

            const auto x0 = static_cast<int>(std::floor(srcX));
            const auto y0 = static_cast<int>(std::floor(srcY));
            const auto wx = srcX - static_cast<float>(x0);
            const auto wy = srcY - static_cast<float>(y0);
            const auto oneMinusWx = 1.f - wx;
            const auto oneMinusWy = 1.f - wy;

            const auto x0c = clampi(x0, 0, maxX);
            const auto x1c = clampi(x0 + 1, 0, maxX);
            const auto y0c = clampi(y0, 0, maxY);
            const auto y1c = clampi(y0 + 1, 0, maxY);

            const auto* rowTop = in + static_cast<std::ptrdiff_t>(y0c) * srcW * 4;
            const auto* rowBot = in + static_cast<std::ptrdiff_t>(y1c) * srcW * 4;

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

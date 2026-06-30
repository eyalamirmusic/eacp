#pragma once

#include <eacp/SIMD/Kernels/Bilinear.h>

#include <cmath>
#include <cstddef>
#include <cstdint>

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
    const int maxX = srcW - 1;
    const int maxY = srcH - 1;

    for (int dy = 0; dy < dstH; ++dy)
    {
        const float fdy = static_cast<float>(dy);

        for (int dx = 0; dx < dstW; ++dx)
        {
            const float fdx = static_cast<float>(dx);
            const float srcX = m[0] * fdx + m[1] * fdy + m[2];
            const float srcY = m[3] * fdx + m[4] * fdy + m[5];

            const int x0 = static_cast<int>(std::floor(srcX));
            const int y0 = static_cast<int>(std::floor(srcY));
            const float wx = srcX - static_cast<float>(x0);
            const float wy = srcY - static_cast<float>(y0);
            const float oneMinusWx = 1.f - wx;
            const float oneMinusWy = 1.f - wy;

            const int x0c = clampi(x0, 0, maxX);
            const int x1c = clampi(x0 + 1, 0, maxX);
            const int y0c = clampi(y0, 0, maxY);
            const int y1c = clampi(y0 + 1, 0, maxY);

            const std::uint8_t* rowTop =
                in + static_cast<std::ptrdiff_t>(y0c) * srcW * 4;
            const std::uint8_t* rowBot =
                in + static_cast<std::ptrdiff_t>(y1c) * srcW * 4;

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

#pragma once

#include "Image.h"

// Bilinear resampling reproducing OpenCV's exact semantics, on straight-alpha
// 8-bit RGBA.
namespace eacp::Graphics
{

// A 2x3 affine matrix, row-major: [ m[0] m[1] m[2] ; m[3] m[4] m[5] ].
struct Affine2x3
{
    float m[6] = {1.f, 0.f, 0.f, 0.f, 1.f, 0.f};
};

// Matches cv::resize(..., INTER_LINEAR): source coordinate for destination
// pixel d is (d + 0.5) * srcSize / dstSize - 0.5, out-of-range taps clamped.
Image resizeBilinear(const Image& src, int dstWidth, int dstHeight);

// Matches cv::warpAffine(..., INTER_LINEAR | WARP_INVERSE_MAP,
// BORDER_REPLICATE). `inverse` maps a destination pixel directly to the source
// coordinate it samples: srcX = m0*dx + m1*dy + m2, srcY = m3*dx + m4*dy + m5.
Image warpAffineInverse(const Image& src,
                        const Affine2x3& inverse,
                        int dstWidth,
                        int dstHeight);

// Crops and mirrors horizontally in one pass. Invalid image when the rectangle
// is not fully inside src.
Image mirroredCrop(const Image& src, int x, int y, int width, int height);

// Reuse overloads: recycle `dst`'s storage, with no allocation when it already
// holds the destination size. `dst` must not alias `src`.
void resizeBilinear(const Image& src, int dstWidth, int dstHeight, Image& dst);
void mirroredCrop(const Image& src, int x, int y, int width, int height, Image& dst);

} // namespace eacp::Graphics

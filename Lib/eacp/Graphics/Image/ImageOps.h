#pragma once

#include <eacp/Graphics/Image/Image.h>

// Bilinear resampling on Graphics::Image, reproducing the exact OpenCV semantics
// that image-processing pipelines (e.g. ML preprocessing) are validated against,
// so they can drop the OpenCV dependency: cv::resize INTER_LINEAR with half-pixel
// centres, and cv::warpAffine INTER_LINEAR | WARP_INVERSE_MAP with
// BORDER_REPLICATE. Both operate on straight-alpha 8-bit RGBA.
namespace eacp::Graphics
{

// A 2x3 affine matrix, row-major: [ m[0] m[1] m[2] ; m[3] m[4] m[5] ].
struct Affine2x3
{
    float m[6] = {1.f, 0.f, 0.f, 0.f, 1.f, 0.f};
};

// Bilinear resize matching cv::resize(..., INTER_LINEAR): the source coordinate
// for a destination pixel d is (d + 0.5) * srcSize / dstSize - 0.5, with
// out-of-range taps edge-clamped. Returns an invalid image for an empty source
// or non-positive size.
Image resizeBilinear(const Image& src, int dstWidth, int dstHeight);

// Affine warp matching cv::warpAffine(..., INTER_LINEAR | WARP_INVERSE_MAP,
// BORDER_REPLICATE). `inverse` maps a destination pixel (dx, dy) DIRECTLY to the
// source coordinate it samples: srcX = m0*dx + m1*dy + m2, srcY = m3*dx + m4*dy +
// m5. Bilinear, with the four taps edge-clamped (border replicate).
Image warpAffineInverse(const Image& src,
                        const Affine2x3& inverse,
                        int dstWidth,
                        int dstHeight);

// Crop the rectangle (x, y, width, height) out of src and mirror it horizontally
// (selfie view) in a single pass. Mirroring commutes with a centred crop, so this
// equals mirroring the whole source then cropping, at a fraction of the per-pixel
// work. Returns an invalid image if the rectangle is not fully inside src.
Image mirroredCrop(const Image& src, int x, int y, int width, int height);

} // namespace eacp::Graphics

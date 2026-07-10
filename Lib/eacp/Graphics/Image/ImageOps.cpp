#include <eacp/Graphics/Image/ImageOps.h>

#include <eacp/SIMD/SIMD.h>

#include <utility>

namespace eacp::Graphics
{

void resizeBilinear(const Image& src, int dstWidth, int dstHeight, Image& dst)
{
    if (!src.isValid() || dstWidth <= 0 || dstHeight <= 0)
    {
        dst = {};
        return;
    }

    auto* out = dst.prepareForOverwrite(dstWidth, dstHeight);
    eacp::simd::resizeBilinear(src.pixels().data(),
                               src.width(),
                               src.height(),
                               out,
                               dstWidth,
                               dstHeight);
}

Image resizeBilinear(const Image& src, int dstWidth, int dstHeight)
{
    auto dst = Image {};
    resizeBilinear(src, dstWidth, dstHeight, dst);
    return dst;
}

Image warpAffineInverse(const Image& src,
                        const Affine2x3& inverse,
                        int dstWidth,
                        int dstHeight)
{
    if (!src.isValid() || dstWidth <= 0 || dstHeight <= 0)
        return {};

    auto outData = ImageData(dstWidth * dstHeight * 4);
    eacp::simd::warpAffineInverse(src.pixels().data(),
                                  src.width(),
                                  src.height(),
                                  inverse.m,
                                  outData.data(),
                                  dstWidth,
                                  dstHeight);
    return {dstWidth, dstHeight, std::move(outData)};
}

void mirroredCrop(
    const Image& src, int x, int y, int width, int height, Image& dst)
{
    if (!src.isValid() || width <= 0 || height <= 0 || x < 0 || y < 0
        || x + width > src.width() || y + height > src.height())
    {
        dst = {};
        return;
    }

    auto* out = dst.prepareForOverwrite(width, height);
    eacp::simd::mirroredCrop(
        src.pixels().data(), src.width(), x, y, width, height, out);
}

Image mirroredCrop(const Image& src, int x, int y, int width, int height)
{
    auto dst = Image {};
    mirroredCrop(src, x, y, width, height, dst);
    return dst;
}

} // namespace eacp::Graphics

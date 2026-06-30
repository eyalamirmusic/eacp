#include <eacp/Graphics/Image/ImageOps.h>

#include <eacp/SIMD/SIMD.h>

#include <utility>

namespace eacp::Graphics
{

Image resizeBilinear(const Image& src, int dstWidth, int dstHeight)
{
    if (!src.isValid() || dstWidth <= 0 || dstHeight <= 0)
        return {};

    auto outData = ImageData(dstWidth * dstHeight * 4);
    eacp::simd::resizeBilinear(src.pixels().data(),
                               src.width(),
                               src.height(),
                               outData.data(),
                               dstWidth,
                               dstHeight);
    return {dstWidth, dstHeight, std::move(outData)};
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

Image mirroredCrop(const Image& src, int x, int y, int width, int height)
{
    if (!src.isValid() || width <= 0 || height <= 0)
        return {};

    const auto srcW = src.width();
    const auto srcH = src.height();

    if (x < 0 || y < 0 || x + width > srcW || y + height > srcH)
        return {};

    auto outData = ImageData(width * height * 4);
    eacp::simd::mirroredCrop(
        src.pixels().data(), srcW, x, y, width, height, outData.data());
    return {width, height, std::move(outData)};
}

} // namespace eacp::Graphics

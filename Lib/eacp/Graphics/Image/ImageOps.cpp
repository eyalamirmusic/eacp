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
    const auto* in = src.pixels().data();
    auto* out = outData.data();

    for (int dy = 0; dy < height; ++dy)
    {
        const std::uint8_t* srcRow =
            in + (static_cast<std::size_t>(y + dy) * srcW + x) * 4;
        std::uint8_t* dstRow = out + static_cast<std::size_t>(dy) * width * 4;
        for (int dx = 0; dx < width; ++dx)
            std::memcpy(dstRow + dx * 4, srcRow + (width - 1 - dx) * 4, 4);
    }
    return {width, height, std::move(outData)};
}

} // namespace eacp::Graphics

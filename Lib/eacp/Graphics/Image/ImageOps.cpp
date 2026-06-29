#include <eacp/Graphics/Image/ImageOps.h>

#include <utility>

namespace eacp::Graphics
{
namespace
{

int clampi(int v, int lo, int hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

// Round half away from zero (values are non-negative bilinear blends of bytes),
// then clamp: the rounded result can land a hair above 255.
std::uint8_t toU8(float v)
{
    return static_cast<std::uint8_t>(
        clampi(static_cast<int>(std::lround(v)), 0, 255));
}

float blend(const std::uint8_t* p00,
            const std::uint8_t* p10,
            const std::uint8_t* p01,
            const std::uint8_t* p11,
            float w00,
            float w10,
            float w01,
            float w11,
            int channel)
{
    return w00 * static_cast<float>(p00[channel])
           + w10 * static_cast<float>(p10[channel])
           + w01 * static_cast<float>(p01[channel])
           + w11 * static_cast<float>(p11[channel]);
}

} // namespace

Image resizeBilinear(const Image& src, int dstWidth, int dstHeight)
{
    if (!src.isValid() || dstWidth <= 0 || dstHeight <= 0)
        return {};

    const int srcW = src.width();
    const int srcH = src.height();
    const std::uint8_t* in = src.pixels().data();
    ImageData outData(dstWidth * dstHeight * 4);
    std::uint8_t* out = outData.data();

    const float scaleX = static_cast<float>(srcW) / static_cast<float>(dstWidth);
    const float scaleY = static_cast<float>(srcH) / static_cast<float>(dstHeight);
    const int maxX = srcW - 1;
    const int maxY = srcH - 1;

    for (int dy = 0; dy < dstHeight; ++dy)
    {
        const float fy = (static_cast<float>(dy) + 0.5f) * scaleY - 0.5f;
        const int y0 = static_cast<int>(std::floor(fy));
        const float wy = fy - static_cast<float>(y0);
        const std::uint8_t* rowTop =
            in + static_cast<std::ptrdiff_t>(clampi(y0, 0, maxY)) * srcW * 4;
        const std::uint8_t* rowBot =
            in + static_cast<std::ptrdiff_t>(clampi(y0 + 1, 0, maxY)) * srcW * 4;

        for (int dx = 0; dx < dstWidth; ++dx)
        {
            const float fx = (static_cast<float>(dx) + 0.5f) * scaleX - 0.5f;
            const int x0 = static_cast<int>(std::floor(fx));
            const float wx = fx - static_cast<float>(x0);
            const int x0c = clampi(x0, 0, maxX);
            const int x1c = clampi(x0 + 1, 0, maxX);

            const float w00 = (1.f - wx) * (1.f - wy);
            const float w10 = wx * (1.f - wy);
            const float w01 = (1.f - wx) * wy;
            const float w11 = wx * wy;

            const std::uint8_t* p00 = rowTop + x0c * 4;
            const std::uint8_t* p10 = rowTop + x1c * 4;
            const std::uint8_t* p01 = rowBot + x0c * 4;
            const std::uint8_t* p11 = rowBot + x1c * 4;

            for (int c = 0; c < 4; ++c)
                out[c] = toU8(blend(p00, p10, p01, p11, w00, w10, w01, w11, c));
            out += 4;
        }
    }
    return {dstWidth, dstHeight, std::move(outData)};
}

Image warpAffineInverse(const Image& src,
                        const Affine2x3& inverse,
                        int dstWidth,
                        int dstHeight)
{
    if (!src.isValid() || dstWidth <= 0 || dstHeight <= 0)
        return {};

    const int srcW = src.width();
    const int srcH = src.height();
    const std::uint8_t* in = src.pixels().data();
    auto outData = ImageData(dstWidth * dstHeight * 4);
    auto* out = outData.data();

    const int maxX = srcW - 1;
    const int maxY = srcH - 1;
    const float* m = inverse.m;

    for (int dy = 0; dy < dstHeight; ++dy)
    {
        for (int dx = 0; dx < dstWidth; ++dx)
        {
            const float fdx = static_cast<float>(dx);
            const float fdy = static_cast<float>(dy);
            const float srcX = m[0] * fdx + m[1] * fdy + m[2];
            const float srcY = m[3] * fdx + m[4] * fdy + m[5];

            const int x0 = static_cast<int>(std::floor(srcX));
            const int y0 = static_cast<int>(std::floor(srcY));
            const float wx = srcX - static_cast<float>(x0);
            const float wy = srcY - static_cast<float>(y0);

            const int x0c = clampi(x0, 0, maxX);
            const int x1c = clampi(x0 + 1, 0, maxX);
            const int y0c = clampi(y0, 0, maxY);
            const int y1c = clampi(y0 + 1, 0, maxY);

            const float w00 = (1.f - wx) * (1.f - wy);
            const float w10 = wx * (1.f - wy);
            const float w01 = (1.f - wx) * wy;
            const float w11 = wx * wy;

            const std::uint8_t* rowTop =
                in + static_cast<std::ptrdiff_t>(y0c) * srcW * 4;
            const std::uint8_t* rowBot =
                in + static_cast<std::ptrdiff_t>(y1c) * srcW * 4;
            const std::uint8_t* p00 = rowTop + x0c * 4;
            const std::uint8_t* p10 = rowTop + x1c * 4;
            const std::uint8_t* p01 = rowBot + x0c * 4;
            const std::uint8_t* p11 = rowBot + x1c * 4;

            for (int c = 0; c < 4; ++c)
                out[c] = toU8(blend(p00, p10, p01, p11, w00, w10, w01, w11, c));
            out += 4;
        }
    }
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

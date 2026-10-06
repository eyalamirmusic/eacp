#include "Camera.h"

#include <eacp/Graphics/Graphics.h>
#include <ESIMD/ESIMD.h>

#include <algorithm>

// Portable Camera members. The platform backends (Camera-macOS.mm /
// Camera-Windows.cpp / Camera-Android.cpp) own the capture session and frame
// delivery; conversions that only touch the public frame fields live here so
// they compile once for every platform.

namespace eacp::Cameras
{
namespace
{
// BGRA (camera byte order) → RGBA (Graphics::Image byte order), into `out`'s
// reused storage. The per-pixel swap + row-unpad runs in ESIMD (always
// optimized); prepareForOverwrite recycles the buffer so a per-frame capture
// loop neither reallocates nor zero-fills. `out` is left empty on a bad size.
void bgraToImage(const std::uint8_t* data,
                 int width,
                 int height,
                 int bytesPerRow,
                 Graphics::Image& out)
{
    auto* dst = out.prepareForOverwrite(width, height);
    if (dst == nullptr)
        return;

    esimd::convertBgraToRgba(data, bytesPerRow, dst, width, height);
}

struct YuvCoefficients
{
    float lumaOffset = 0.0f;
    float lumaScale = 1.0f;
    float chromaScale = 1.0f;
    float redV = 0.0f;
    float greenU = 0.0f;
    float greenV = 0.0f;
    float blueU = 0.0f;
};

YuvCoefficients coefficientsFor(YuvMatrix matrix, bool fullRange)
{
    auto kr = matrix == YuvMatrix::BT709 ? 0.2126f : 0.299f;
    auto kb = matrix == YuvMatrix::BT709 ? 0.0722f : 0.114f;
    auto kg = 1.0f - kr - kb;

    auto result = YuvCoefficients {};
    result.lumaOffset = fullRange ? 0.0f : 16.0f;
    result.lumaScale = fullRange ? 1.0f : 255.0f / 219.0f;
    result.chromaScale = fullRange ? 1.0f : 255.0f / 224.0f;
    result.redV = 2.0f * (1.0f - kr);
    result.greenU = 2.0f * kb * (1.0f - kb) / kg;
    result.greenV = 2.0f * kr * (1.0f - kr) / kg;
    result.blueU = 2.0f * (1.0f - kb);
    return result;
}

std::uint8_t toByte(float value)
{
    return (std::uint8_t) std::clamp(value + 0.5f, 0.0f, 255.0f);
}

void nv12ToImage(const CameraFrame& frame, Graphics::Image& out)
{
    auto width = frame.width();
    auto height = frame.height();

    if (width < 2 || height < 2)
    {
        out = {};
        return;
    }

    auto* dst = out.prepareForOverwrite(width, height);

    if (dst == nullptr)
        return;

    auto lastPair = (width / 2 - 1) * 2;
    auto lastChromaRow = height / 2 - 1;

    auto k = coefficientsFor(frame.yuvMatrix(), frame.fullRangeYuv());
    auto stride = frame.bytesPerRow();
    const auto* luma = frame.data();
    const auto* chroma = frame.chromaPlane();

    for (auto y = 0; y < height; ++y)
    {
        const auto* lumaRow = luma + y * stride;
        const auto* chromaRow = chroma + std::min(y / 2, lastChromaRow) * stride;
        auto* outRow = dst + y * width * 4;

        for (auto x = 0; x < width; ++x)
        {
            auto pair = std::min((x / 2) * 2, lastPair);
            auto l = ((float) lumaRow[x] - k.lumaOffset) * k.lumaScale;
            auto u = ((float) chromaRow[pair] - 128.0f) * k.chromaScale;
            auto v = ((float) chromaRow[pair + 1] - 128.0f) * k.chromaScale;

            auto* pixel = outRow + x * 4;
            pixel[0] = toByte(l + k.redV * v);
            pixel[1] = toByte(l - k.greenU * u - k.greenV * v);
            pixel[2] = toByte(l + k.blueU * u);
            pixel[3] = 255;
        }
    }
}
} // namespace

CameraFrame::CameraFrame(int width,
                         int height,
                         PixelFormat format,
                         int bytesPerRow,
                         double timestampSeconds,
                         const std::uint8_t* data,
                         void* nativeBuffer,
                         int rotationDegrees,
                         YuvMatrix yuvMatrix,
                         bool fullRangeYuv)
    : frameWidth(width)
    , frameHeight(height)
    , pixelFormat(format)
    , rowBytes(bytesPerRow)
    , timestamp(timestampSeconds)
    , pixels(data)
    , buffer(nativeBuffer)
    , rotation(rotationDegrees)
    , matrix(yuvMatrix)
    , fullRange(fullRangeYuv)
{
}

const std::uint8_t* CameraFrame::chromaPlane() const
{
    if (pixels == nullptr || pixelFormat != PixelFormat::NV12)
        return nullptr;

    return pixels + (std::size_t) rowBytes * (std::size_t) frameHeight;
}

Graphics::Image CameraFrame::toImage() const
{
    auto image = Graphics::Image {};
    toImage(image);
    return image;
}

void CameraFrame::toImage(Graphics::Image& reuse) const
{
    if (pixels == nullptr || frameWidth <= 0 || frameHeight <= 0)
    {
        reuse = {};
        return;
    }

    if (pixelFormat == PixelFormat::NV12)
        nv12ToImage(*this, reuse);
    else
        bgraToImage(pixels, frameWidth, frameHeight, rowBytes, reuse);
}
} // namespace eacp::Cameras

#include "Camera.h"

#include <eacp/Graphics/Graphics.h>
#include <eacp/SIMD/SIMD.h>

namespace eacp::Cameras
{
namespace
{
// BGRA (camera order) to RGBA (Image order), unpadding rows into `out`'s
// recycled storage. `out` is left empty on a bad size.
void bgraToImage(const std::uint8_t* data,
                 int width,
                 int height,
                 std::size_t bytesPerRow,
                 Graphics::Image& out)
{
    auto* dst = out.prepareForOverwrite(width, height);
    if (dst == nullptr)
        return;

    eacp::simd::convertBgraToRgba(data, bytesPerRow, dst, width, height);
}
} // namespace

CameraFrame::CameraFrame(int width,
                         int height,
                         PixelFormat format,
                         std::size_t bytesPerRow,
                         double timestampSeconds,
                         const std::uint8_t* data,
                         void* nativeBuffer)
    : frameWidth(width)
    , frameHeight(height)
    , pixelFormat(format)
    , rowBytes(bytesPerRow)
    , timestamp(timestampSeconds)
    , pixels(data)
    , buffer(nativeBuffer)
{
}

Graphics::Image CameraFrame::toImage() const
{
    auto image = Graphics::Image {};
    toImage(image);
    return image;
}

void CameraFrame::toImage(Graphics::Image& reuse) const
{
    if (pixels == nullptr || frameWidth <= 0 || frameHeight <= 0
        || pixelFormat != PixelFormat::BGRA8)
    {
        reuse = {};
        return;
    }

    bgraToImage(pixels, frameWidth, frameHeight, rowBytes, reuse);
}
} // namespace eacp::Cameras

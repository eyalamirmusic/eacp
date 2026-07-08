#include "Camera.h"

#include <eacp/Graphics/Image/Image.h>
#include <eacp/SIMD/SIMD.h>

// Portable Camera members. The platform backends (Camera-macOS.mm /
// Camera-Windows.cpp) own the capture session and frame delivery; conversions
// that only touch the public frame fields live here so they compile once for
// every platform.

namespace eacp::Cameras
{
namespace
{
// BGRA (camera byte order) → RGBA (Graphics::Image byte order), into `out`'s
// reused storage. The per-pixel swap + row-unpad runs in eacp-simd (always
// optimized); prepareForOverwrite recycles the buffer so a per-frame capture
// loop neither reallocates nor zero-fills. `out` is left empty on a bad size.
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
        // NV12 / other planar formats land in a later phase; until then an
        // unreadable or unsupported frame yields an empty image.
        reuse = {};
        return;
    }

    bgraToImage(pixels, frameWidth, frameHeight, rowBytes, reuse);
}
} // namespace eacp::Cameras

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
// BGRA (camera byte order) → RGBA (Graphics::Image byte order): the SIMD
// red/blue swap does the per-pixel work, dropping any trailing row padding.
Graphics::Image bgraToImage(const std::uint8_t* data,
                            int width,
                            int height,
                            std::size_t bytesPerRow)
{
    auto rgba = Graphics::ImageData {};
    rgba.resize((std::size_t) width * height * 4);

    const auto rowPixels = (std::size_t) width;
    const auto tightRowBytes = rowPixels * 4;

    // Tightly-packed frames swap in a single pass; padded rows go one by one.
    if (bytesPerRow == tightRowBytes)
    {
        eacp::simd::swapRedBlue(data, rgba.data(), rowPixels * (std::size_t) height);
    }
    else
    {
        for (auto y = 0; y < height; ++y)
            eacp::simd::swapRedBlue(data + (std::size_t) y * bytesPerRow,
                                    rgba.data() + (std::size_t) y * tightRowBytes,
                                    rowPixels);
    }

    return Graphics::Image(width, height, std::move(rgba));
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
    if (pixels == nullptr || frameWidth <= 0 || frameHeight <= 0)
        return {};

    if (pixelFormat == PixelFormat::BGRA8)
        return bgraToImage(pixels, frameWidth, frameHeight, rowBytes);

    // NV12 / other planar formats: conversion lands in a later phase.
    return {};
}
} // namespace eacp::Cameras

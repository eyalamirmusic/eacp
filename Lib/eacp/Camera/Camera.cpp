#include "Camera.h"

#include <eacp/Graphics/Image/Image.h>

// Portable Camera members. The platform backends (Camera-macOS.mm /
// Camera-Windows.cpp) own the capture session and frame delivery; conversions
// that only touch the public frame fields live here so they compile once for
// every platform.

namespace eacp::Cameras
{
namespace
{
// BGRA (camera byte order) → RGBA (Graphics::Image byte order), copying
// width * 4 bytes per row and dropping any trailing row padding.
Graphics::Image bgraToImage(const std::uint8_t* data,
                            int width,
                            int height,
                            std::size_t bytesPerRow)
{
    auto rgba = Graphics::ImageData {};
    rgba.resize((std::size_t) width * height * 4);

    for (auto y = 0; y < height; ++y)
    {
        const auto* source = data + (std::size_t) y * bytesPerRow;
        auto* destination = rgba.data() + (std::size_t) y * width * 4;

        for (auto x = 0; x < width; ++x)
        {
            destination[x * 4 + 0] = source[x * 4 + 2]; // R ← B
            destination[x * 4 + 1] = source[x * 4 + 1]; // G
            destination[x * 4 + 2] = source[x * 4 + 0]; // B ← R
            destination[x * 4 + 3] = source[x * 4 + 3]; // A
        }
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

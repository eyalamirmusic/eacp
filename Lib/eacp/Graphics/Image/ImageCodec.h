#pragma once

#include "Image.h"

#include <cstdint>
#include <string>

// Internal seam between the cross-platform Image logic (Image.cpp) and
// the per-platform codecs (Image-Apple.mm, Image-Windows.cpp). Not part
// of the public Graphics surface.
namespace eacp::Graphics::detail
{

// Decode PNG/JPEG bytes into a straight-alpha 8-bit RGBA Image. On
// malformed or unsupported input returns an invalid image (see
// Image::operator bool) and sets error.
Image decodeImageBytes(const std::uint8_t* data, int size, std::string& error);

// Encode tightly packed straight-alpha 8-bit RGBA to PNG/JPEG bytes.
// Returns an empty buffer and sets error on failure.
ImageData encodeImageBytes(const std::uint8_t* rgba,
                           int width,
                           int height,
                           ImageFormat format,
                           float quality,
                           std::string& error);

} // namespace eacp::Graphics::detail

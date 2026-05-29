#pragma once

#include "Image.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

// Internal seam between the cross-platform Image logic (Image.cpp) and
// the per-platform codecs (Image-Apple.mm, Image-Windows.cpp). Not part
// of the public Graphics surface.
namespace eacp::Graphics::detail
{

struct DecodedImage
{
    int width = 0;
    int height = 0;
    std::vector<std::uint8_t> rgba;
};

// Decode PNG/JPEG bytes to tightly packed straight-alpha 8-bit RGBA.
// Returns std::nullopt and sets error on failure.
std::optional<DecodedImage>
    decodeImageBytes(const std::uint8_t* data, std::size_t size, std::string& error);

// Encode tightly packed straight-alpha 8-bit RGBA to PNG/JPEG bytes.
// Returns an empty vector and sets error on failure.
std::vector<std::uint8_t> encodeImageBytes(const std::uint8_t* rgba,
                                           int width,
                                           int height,
                                           ImageFormat format,
                                           float quality,
                                           std::string& error);

} // namespace eacp::Graphics::detail

#pragma once

#include "Image.h"

// Internal seam between Image.cpp and the per-platform codecs. Not public.
namespace eacp::Graphics::detail
{

// Returns an invalid image and sets error on malformed input.
Image decodeImageBytes(const std::uint8_t* data, int size, std::string& error);

// Returns an empty buffer and sets error on failure.
ImageData encodeImageBytes(const std::uint8_t* rgba,
                           int width,
                           int height,
                           ImageFormat format,
                           float quality,
                           std::string& error);

} // namespace eacp::Graphics::detail

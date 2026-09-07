#include "../Image/ImageCodec.h"

namespace eacp::Graphics::detail
{
// No PNG/JPEG codec on Linux; both report failure the way Image.h documents.
Image decodeImageBytes(const std::uint8_t*, int, std::string& error)
{
    error = "no image codec on Linux";
    return {};
}

ImageData encodeImageBytes(
    const std::uint8_t*, int, int, ImageFormat, float, std::string& error)
{
    error = "no image codec on Linux";
    return {};
}
} // namespace eacp::Graphics::detail

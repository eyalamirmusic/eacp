#include "../Image/ImageCodec.h"

namespace eacp::Graphics::detail
{
// No PNG/JPEG codec on Linux. macOS gets one from ImageIO and Windows from WIC;
// there is no equivalent to reach for here, so the choice is a vendored decoder
// or nothing, and nothing is what the module contract already covers: decode
// and load report failure by returning an invalid Image with the error set
// (Image.h), which is exactly what a caller has to handle for a corrupt file
// anyway. Encoding throws, as Image::encode documents for a codec failure.
//
// Everything Image does without a codec still works: construction, pixel
// access, prepareForOverwrite, comparison, and the whole of ImageOps — which is
// what the GPU read-back path and the SIMD image kernels use it for.
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

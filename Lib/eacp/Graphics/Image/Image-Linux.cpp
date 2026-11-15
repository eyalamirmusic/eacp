#include "../Image/ImageCodec.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <vector>

#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#define STBI_NO_STDIO
#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>

#define STBI_WRITE_NO_STDIO
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image_write.h>

namespace eacp::Graphics::detail
{
namespace
{
struct DecodedPixels
{
    explicit DecodedPixels(stbi_uc* pixelsToUse)
        : pixels(pixelsToUse)
    {
    }

    ~DecodedPixels() { stbi_image_free(pixels); }

    DecodedPixels(const DecodedPixels&) = delete;
    DecodedPixels& operator=(const DecodedPixels&) = delete;

    stbi_uc* pixels;
};

bool fitsInAnImage(int width, int height)
{
    constexpr auto maxPixels = std::numeric_limits<int>::max() / 4;
    return width > 0 && height > 0 && height <= maxPixels / width;
}

void appendEncodedBytes(void* context, void* data, int size)
{
    auto& bytes = *static_cast<std::vector<std::uint8_t>*>(context);
    auto* first = static_cast<const std::uint8_t*>(data);
    bytes.insert(bytes.end(), first, first + size);
}

int toJpegQuality(float quality)
{
    return std::clamp(static_cast<int>(std::lround(quality * 100.f)), 1, 100);
}
} // namespace

Image decodeImageBytes(const std::uint8_t* data, int size, std::string& error)
{
    if (data == nullptr || size <= 0)
    {
        error = "empty image data";
        return {};
    }

    auto width = 0;
    auto height = 0;
    auto channels = 0;
    auto decoded = DecodedPixels {
        stbi_load_from_memory(data, size, &width, &height, &channels, 4)};

    if (decoded.pixels == nullptr)
    {
        error = std::string {"could not decode image: "} + stbi_failure_reason();
        return {};
    }

    if (!fitsInAnImage(width, height))
    {
        error = "decoded image is too large";
        return {};
    }

    auto rgba = ImageData(width * height * 4);
    std::memcpy(rgba.data(), decoded.pixels, static_cast<std::size_t>(rgba.size()));
    return Image(width, height, std::move(rgba));
}

ImageData encodeImageBytes(const std::uint8_t* rgba,
                           int width,
                           int height,
                           ImageFormat format,
                           float quality,
                           std::string& error)
{
    if (rgba == nullptr || !fitsInAnImage(width, height))
    {
        error = "nothing to encode";
        return {};
    }

    auto encoded = std::vector<std::uint8_t> {};
    auto written =
        format == ImageFormat::png
            ? stbi_write_png_to_func(
                  appendEncodedBytes, &encoded, width, height, 4, rgba, width * 4)
            : stbi_write_jpg_to_func(appendEncodedBytes,
                                     &encoded,
                                     width,
                                     height,
                                     4,
                                     rgba,
                                     toJpegQuality(quality));

    if (written == 0 || encoded.empty())
    {
        error = "image encoding failed";
        return {};
    }

    if (encoded.size() > static_cast<std::size_t>(std::numeric_limits<int>::max()))
    {
        error = "encoded image is too large";
        return {};
    }

    auto bytes = ImageData(static_cast<int>(encoded.size()));
    std::memcpy(bytes.data(), encoded.data(), encoded.size());
    return bytes;
}
} // namespace eacp::Graphics::detail

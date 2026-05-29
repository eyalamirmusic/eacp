#pragma once

#include <eacp/Graphics/Primitives/Primitives.h>

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace eacp::Graphics
{

enum class ImageFormat
{
    png,
    jpeg
};

// A decoded raster image held as tightly packed 8-bit RGBA: exactly
// width * height * 4 bytes, no row padding, top-left origin, straight
// (non-premultiplied) alpha. Decode from / encode to PNG or JPEG via
// the platform image codecs (ImageIO on Apple, WIC on Windows).
//
// Pixel access never throws and decoding untrusted bytes returns
// std::nullopt on malformed input. The size/dimension constructors throw
// std::invalid_argument on a negative dimension or a pixel buffer whose
// length does not match width * height * 4. Encoding a valid image and
// writing to disk throw std::runtime_error on failure, since a valid
// image is always encodable.
class Image
{
public:
    Image() = default;

    // Zero-filled (fully transparent) image of the given size.
    Image(int widthToUse, int heightToUse);

    // Adopts an existing RGBA buffer. Throws std::invalid_argument if
    // pixels.size() != width * height * 4.
    Image(int widthToUse, int heightToUse, std::vector<std::uint8_t> pixelsToUse);

    // Decode PNG or JPEG; the format is detected from the byte
    // signature by the platform codec. Returns std::nullopt on
    // malformed or unsupported input, setting *error when provided.
    static std::optional<Image> decode(const std::uint8_t* data,
                                       std::size_t size,
                                       std::string* error = nullptr);
    static std::optional<Image> decode(const std::vector<std::uint8_t>& bytes,
                                       std::string* error = nullptr);

    // Read a file and decode it. Returns std::nullopt if the file
    // cannot be read or its contents do not decode.
    static std::optional<Image> load(const std::filesystem::path& path,
                                     std::string* error = nullptr);

    bool isValid() const;
    bool isEmpty() const;
    int width() const;
    int height() const;
    const std::vector<std::uint8_t>& pixels() const;

    // (x, y) from the top-left. Out-of-range reads return transparent
    // black; out-of-range writes are ignored.
    Color at(int x, int y) const;
    void set(int x, int y, const Color& color);

    // Encode into an in-memory buffer. quality (0..1) applies to JPEG
    // only and is ignored for PNG. Throws std::runtime_error on an
    // empty/invalid image or codec failure.
    std::vector<std::uint8_t> encode(ImageFormat format, float quality = 0.9f) const;
    std::vector<std::uint8_t> toPng() const;
    std::vector<std::uint8_t> toJpeg(float quality = 0.9f) const;

    // Encode and write to disk. The single-argument form infers the
    // format from the path extension (.png / .jpg / .jpeg). Creates
    // parent directories. Throws on an unknown extension or IO failure.
    void save(const std::filesystem::path& path) const;
    void save(const std::filesystem::path& path,
              ImageFormat format,
              float quality = 0.9f) const;

    // Exact comparison: identical dimensions and identical pixels.
    bool equals(const Image& other) const;
    bool operator==(const Image& other) const;
    bool operator!=(const Image& other) const;

private:
    int w = 0;
    int h = 0;
    std::vector<std::uint8_t> rgba;
};

} // namespace eacp::Graphics

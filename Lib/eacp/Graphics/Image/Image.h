#pragma once

#include <eacp/Graphics/Primitives/Primitives.h>
#include <filesystem>

namespace eacp::Graphics
{

// Tightly packed 8-bit RGBA pixel storage (R,G,B,A per pixel).
using ImageData = Vector<std::uint8_t>;

enum class ImageFormat
{
    png,
    jpeg
};

// A decoded raster image held as tightly packed 8-bit RGBA: exactly
// width * height * 4 bytes, no row padding, top-left origin, straight
// (non-premultiplied) alpha. Decodes from / encodes to PNG or JPEG.
//
// Pixel access never throws. decode()/load() report failure by returning
// an invalid image (operator bool / isValid() is false), setting *error
// when provided. The size/dimension constructors throw std::invalid_argument
// on a negative dimension or a pixel buffer whose length does not match
// width * height * 4. Encoding a valid image or writing it to disk throws
// std::runtime_error on failure.
class Image
{
public:
    Image() = default;

    // Zero-filled (fully transparent) image of the given size.
    Image(int widthToUse, int heightToUse);

    // Adopts an existing RGBA buffer. Throws std::invalid_argument if
    // pixels.size() != width * height * 4.
    Image(int widthToUse, int heightToUse, ImageData pixelsToUse);

    // Decode PNG or JPEG; the format is detected from the byte signature
    // by the platform codec. On malformed or unsupported input returns an
    // invalid image (see operator bool), setting *error when provided.
    static Image
        decode(const std::uint8_t* data, int size, std::string* error = nullptr);
    static Image decode(const ImageData& bytes, std::string* error = nullptr);

    // Read a file and decode it. Returns an invalid image if the file
    // cannot be read or its contents do not decode.
    static Image load(const std::filesystem::path& path,
                      std::string* error = nullptr);

    bool isValid() const;
    bool isEmpty() const;

    // True when the image holds valid pixels; mirrors isValid().
    explicit operator bool() const;

    int width() const;
    int height() const;
    const ImageData& pixels() const;

    // (x, y) from the top-left. Out-of-range reads return transparent
    // black; out-of-range writes are ignored.
    Color at(int x, int y) const;
    void set(int x, int y, const Color& color);

    // Encode into an in-memory buffer. quality (0..1) applies to JPEG
    // only and is ignored for PNG. Throws std::runtime_error on an
    // empty/invalid image or codec failure.
    ImageData encode(ImageFormat format, float quality = 0.9f) const;
    ImageData toPng() const;
    ImageData toJpeg(float quality = 0.9f) const;

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
    ImageData rgba;
};

} // namespace eacp::Graphics

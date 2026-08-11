#pragma once

#include "../Common.h"

#include "../Primitives/Primitives.h"

namespace eacp::Graphics
{

// Tightly packed 8-bit RGBA, no row padding.
using ImageData = Vector<std::uint8_t>;

enum class ImageFormat
{
    png,
    jpeg
};

// width * height * 4 bytes of RGBA, top-left origin, straight (non-
// premultiplied) alpha. Pixel access never throws; decode()/load() report
// failure with an invalid image, the constructors and encoders throw.
class Image
{
public:
    Image() = default;

    // Zero-filled, so fully transparent.
    Image(int widthToUse, int heightToUse);

    // Throws std::invalid_argument unless pixels.size() == width * height * 4.
    Image(int widthToUse, int heightToUse, ImageData pixelsToUse);

    // PNG or JPEG, detected from the byte signature. Returns an invalid image
    // on malformed input, setting *error when provided.
    static Image
        decode(const std::uint8_t* data, int size, std::string* error = nullptr);
    static Image decode(const ImageData& bytes, std::string* error = nullptr);

    // Invalid image when the file cannot be read or does not decode.
    static Image load(const FilePath& path, std::string* error = nullptr);

    bool isValid() const;
    bool isEmpty() const;

    explicit operator bool() const;

    int width() const;
    int height() const;
    const ImageData& pixels() const;

    // (x, y) from the top-left. Out-of-range reads give transparent black;
    // out-of-range writes are ignored.
    Color at(int x, int y) const;
    void set(int x, int y, const Color& color);

    // Reuses this image's storage as a render target for a writer that fills
    // every byte: no zero-fill when the pixel count already matches. Returns
    // the buffer, or nullptr for a non-positive/oversized size.
    std::uint8_t* prepareForOverwrite(int width, int height);

    // quality (0..1) is JPEG-only. Throws std::runtime_error on failure.
    ImageData encode(ImageFormat format, float quality = 0.9f) const;
    ImageData toPng() const;
    ImageData toJpeg(float quality = 0.9f) const;

    // The single-argument form infers the format from the path extension.
    // Creates parent directories. Throws on an unknown extension or IO failure.
    void save(const FilePath& path) const;
    void save(const FilePath& path, ImageFormat format, float quality = 0.9f) const;

    bool equals(const Image& other) const;
    bool operator==(const Image& other) const;
    bool operator!=(const Image& other) const;

private:
    int w = 0;
    int h = 0;
    ImageData rgba;
};

} // namespace eacp::Graphics

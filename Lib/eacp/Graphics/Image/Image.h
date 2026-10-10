#pragma once

#include "../Common.h"

#include "../Primitives/Primitives.h"

#include <memory>

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
//
// Copies share pixels: copying or returning an Image by value shares one
// ref-counted pixel buffer instead of duplicating it, so an Image is as cheap
// to pass around as a handle. A write (set(), prepareForOverwrite()) on an
// image whose buffer is shared first detaches it into a buffer of its own, so
// other copies never see the change. Reads never detach, and pixels() of an
// image holding nothing is an empty buffer. Copies of an Image may be read
// from any thread at once; one Image object must still not be written on one
// thread while it is read or written on another.
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
    static Image load(const FilePath& path, std::string* error = nullptr);

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

    // Reuse this image's storage as a width*height RGBA render target for an
    // external writer that fills every byte (an esimd image kernel, a
    // camera colour-convert, ...). Only (re)allocates when the pixel count
    // changes or the buffer is shared with another copy; when it is unshared
    // and already matches, there is no allocation and no zero-fill -- the
    // previous bytes are left for the writer to overwrite in full. A shared
    // buffer is left to its other copies and replaced by a fresh one without
    // copying the old pixels. This lets a per-frame pipeline recycle one Image
    // instead of allocating (and zero-filling) a fresh buffer each frame, as
    // long as it does not keep copies of it. Returns the writable pixel buffer
    // (width*height*4 bytes), or nullptr for a non-positive / oversized size
    // (leaving the image empty). Prefer the ImageOps reuse overloads and
    // CameraFrame::toImage(Image&) over calling this directly.
    std::uint8_t* prepareForOverwrite(int width, int height);

    // Encode into an in-memory buffer. quality (0..1) applies to JPEG
    // only and is ignored for PNG. Throws std::runtime_error on an
    // empty/invalid image or codec failure.
    ImageData encode(ImageFormat format, float quality = 0.9f) const;
    ImageData toPng() const;
    ImageData toJpeg(float quality = 0.9f) const;

    // Encode and write to disk. The single-argument form infers the
    // format from the path extension (.png / .jpg / .jpeg). Creates
    // parent directories. Throws on an unknown extension or IO failure.
    void save(const FilePath& path) const;
    void save(const FilePath& path, ImageFormat format, float quality = 0.9f) const;

    // Exact comparison: identical dimensions and identical pixels. Copies
    // sharing one buffer compare equal without reading it.
    bool equals(const Image& other) const;
    bool operator==(const Image& other) const;
    bool operator!=(const Image& other) const;

    // The pixel buffer this image reads, as an identity for a cache keyed on it
    // rather than on the pixels: every copy sharing the buffer names the same
    // one, and a write that detaches moves the writer to a new one. Empty for
    // an image holding nothing. Weak, so that keeping it neither keeps the
    // pixels alive nor counts as a copy -- a strong reference would turn every
    // later write into a detach -- and so that once it has expired it says its
    // address may now belong to a different buffer. Lock it only for as long
    // as it takes to read the address.
    std::weak_ptr<const void> pixelBuffer() const;

    // Bumped by every write that changes the buffer in place (set(),
    // prepareForOverwrite() on an unshared image), which a buffer's identity
    // alone cannot show. Together with pixelBuffer() it names these exact
    // pixels without reading them. 0 for an image holding nothing.
    //
    // prepareForOverwrite() bumps it when it is called, not when the writer
    // finishes, so the pointer it returns has to be written in full before the
    // image is next handed to anything keyed on the pair -- a cache that saw
    // the image half-written would keep the half it saw.
    std::uint64_t pixelGeneration() const;

private:
    // The pixels, and how many times they have been written in place.
    struct Buffer
    {
        ImageData pixels;
        std::uint64_t generation = 0;
    };

    ImageData& writablePixels();

    int w = 0;
    int h = 0;
    std::shared_ptr<Buffer> rgba;
};

} // namespace eacp::Graphics

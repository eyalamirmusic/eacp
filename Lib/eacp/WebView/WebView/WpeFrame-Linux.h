#pragma once

#include <eacp/Graphics/Image/Image.h>

#include <cstdint>
#include <vector>

namespace eacp::Graphics::detail
{
// WPE's shared-memory frames are ARGB8888 in memory order: B, G, R, A per
// pixel, premultiplied. An Image is R, G, B, A, straight.
Image imageFromPremultipliedBgra(const std::uint8_t* bgra,
                                 int width,
                                 int height,
                                 int stride);

// The last frame a web view rendered, kept in the form it arrived in so a
// frame nobody snapshots costs one copy and no conversion.
class WpeFrame
{
public:
    void store(const std::uint8_t* bgra, int width, int height, int stride);

    bool isEmpty() const;
    int width() const;
    int height() const;

    // Tightly packed rows, width * 4 bytes each.
    const std::uint8_t* data() const;

    Image toImage() const;

private:
    std::vector<std::uint8_t> pixels;
    int frameWidth = 0;
    int frameHeight = 0;
};
} // namespace eacp::Graphics::detail

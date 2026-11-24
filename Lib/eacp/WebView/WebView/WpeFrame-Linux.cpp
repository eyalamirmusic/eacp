#include "WpeFrame-Linux.h"

#include <algorithm>
#include <cstring>

namespace eacp::Graphics::detail
{
namespace
{
std::uint8_t unpremultiply(std::uint8_t channel, std::uint8_t alpha)
{
    auto straight = ((int) channel * 255 + alpha / 2) / alpha;
    return (std::uint8_t) std::min(straight, 255);
}
} // namespace

Image imageFromPremultipliedBgra(const std::uint8_t* bgra,
                                 int width,
                                 int height,
                                 int stride)
{
    auto image = Image {};
    auto* rgba = image.prepareForOverwrite(width, height);

    if (rgba == nullptr || bgra == nullptr)
        return {};

    for (auto y = 0; y < height; ++y)
    {
        const auto* source = bgra + (std::size_t) y * (std::size_t) stride;
        auto* target = rgba + (std::size_t) y * (std::size_t) width * 4;

        for (auto x = 0; x < width; ++x, source += 4, target += 4)
        {
            auto alpha = source[3];

            if (alpha == 0)
            {
                std::memset(target, 0, 4);
                continue;
            }

            target[0] = unpremultiply(source[2], alpha);
            target[1] = unpremultiply(source[1], alpha);
            target[2] = unpremultiply(source[0], alpha);
            target[3] = alpha;
        }
    }

    return image;
}

void WpeFrame::store(const std::uint8_t* bgra, int width, int height, int stride)
{
    if (bgra == nullptr || width <= 0 || height <= 0 || stride < width * 4)
        return;

    auto rowBytes = (std::size_t) width * 4;
    pixels.resize(rowBytes * (std::size_t) height);

    for (auto y = 0; y < height; ++y)
        std::memcpy(pixels.data() + rowBytes * (std::size_t) y,
                    bgra + (std::size_t) stride * (std::size_t) y,
                    rowBytes);

    frameWidth = width;
    frameHeight = height;
}

bool WpeFrame::isEmpty() const
{
    return pixels.empty();
}

int WpeFrame::width() const
{
    return frameWidth;
}

int WpeFrame::height() const
{
    return frameHeight;
}

const std::uint8_t* WpeFrame::data() const
{
    return pixels.data();
}

Image WpeFrame::toImage() const
{
    if (isEmpty())
        return {};

    return imageFromPremultipliedBgra(
        pixels.data(), frameWidth, frameHeight, frameWidth * 4);
}
} // namespace eacp::Graphics::detail

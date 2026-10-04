#include "Encoder.h"

#include <eacp/Graphics/Image/Image.h>

namespace eacp::Video
{

void compositeOverBlackBGRA(const Graphics::Image& image,
                            std::uint8_t* dst,
                            int width,
                            int height,
                            int dstStride)
{
    const auto* src = image.pixels().data();
    auto srcStride = image.width() * 4;

    for (auto y = 0; y < height; ++y)
    {
        const auto* s = src + y * srcStride;
        auto* d = dst + y * dstStride;

        for (auto x = 0; x < width; ++x)
        {
            auto r = s[x * 4 + 0];
            auto g = s[x * 4 + 1];
            auto b = s[x * 4 + 2];
            auto a = s[x * 4 + 3];

            // Straight RGBA over black -> premultiplied, opaque BGRA.
            auto overBlack = [&](std::uint8_t c) -> std::uint8_t
            { return static_cast<std::uint8_t>((c * a + 127) / 255); };

            d[x * 4 + 0] = overBlack(b);
            d[x * 4 + 1] = overBlack(g);
            d[x * 4 + 2] = overBlack(r);
            d[x * 4 + 3] = 255;
        }
    }
}

void Encoder::waitUntilReady(Time::MS) {}

bool Encoder::canCaptureNativeContent(Graphics::View&, float, int, int)
{
    return false;
}

bool Encoder::appendNativeContent(Graphics::View&, float, double)
{
    return false;
}

} // namespace eacp::Video

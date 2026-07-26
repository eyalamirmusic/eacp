#include "FrameImage.h"

namespace eacp::Video
{
Graphics::Image toImage(const VideoFrame& frame)
{
    if (!frame.isValid() || frame.width() <= 0 || frame.height() <= 0)
        return {};

    if (const auto* pixels = frame.pixels())
    {
        auto image = Graphics::Image {frame.width(), frame.height()};
        auto stride = frame.bytesPerRow() != 0 ? frame.bytesPerRow()
                                               : (std::size_t) frame.width() * 4;

        for (auto y = 0; y < frame.height(); ++y)
        {
            const auto* row = pixels + (std::size_t) y * stride;

            for (auto x = 0; x < frame.width(); ++x)
                image.set(x,
                          y,
                          Graphics::Color {row[x * 4 + 2] / 255.0f,
                                           row[x * 4 + 1] / 255.0f,
                                           row[x * 4 + 0] / 255.0f,
                                           row[x * 4 + 3] / 255.0f});
        }

        return image;
    }

    return nativeBufferToImage(frame.nativeBuffer());
}
} // namespace eacp::Video

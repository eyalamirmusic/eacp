#include "FrameImage.h"

#include <algorithm>

namespace eacp::Video
{
using Graphics::Color;

namespace
{
// The same arithmetic the NV12 shader runs, from the same constants, so a frame
// looks identical whether it went to the screen or through toImage.
Color yuvToColor(const YuvTransform& transform,
                 std::uint8_t luma,
                 std::uint8_t cb,
                 std::uint8_t cr)
{
    auto y = (luma / 255.0f - transform.lumaOffset) * transform.lumaScale;
    auto u = (cb / 255.0f - transform.chromaOffset) * transform.chromaScale;
    auto v = (cr / 255.0f - transform.chromaOffset) * transform.chromaScale;

    auto clamped = [](float value) { return std::clamp(value, 0.0f, 1.0f); };

    return {clamped(y + transform.redV * v),
            clamped(y - transform.greenU * u - transform.greenV * v),
            clamped(y + transform.blueU * u),
            1.0f};
}

Graphics::Image nv12ToImage(const VideoFrame& frame)
{
    const auto* luma = frame.pixels();
    const auto* chroma = frame.chromaPlane();

    if (luma == nullptr || chroma == nullptr)
        return {};

    auto image = Graphics::Image {frame.width(), frame.height()};
    auto stride = frame.bytesPerRow();
    auto transform = frame.yuvTransform();

    for (auto y = 0; y < frame.height(); ++y)
    {
        const auto* lumaRow = luma + (std::size_t) y * stride;

        // One chroma row and one chroma pair per two pixels each way.
        const auto* chromaRow = chroma + (std::size_t) (y / 2) * stride;

        for (auto x = 0; x < frame.width(); ++x)
        {
            const auto* pair = chromaRow + (std::size_t) (x / 2) * 2;
            image.set(x, y, yuvToColor(transform, lumaRow[x], pair[0], pair[1]));
        }
    }

    return image;
}

Graphics::Image bgraToImage(const VideoFrame& frame)
{
    const auto* pixels = frame.pixels();
    auto image = Graphics::Image {frame.width(), frame.height()};
    auto stride = frame.bytesPerRow() != 0 ? frame.bytesPerRow()
                                           : (std::size_t) frame.width() * 4;

    for (auto y = 0; y < frame.height(); ++y)
    {
        const auto* row = pixels + (std::size_t) y * stride;

        for (auto x = 0; x < frame.width(); ++x)
            image.set(x,
                      y,
                      Color {row[x * 4 + 2] / 255.0f,
                             row[x * 4 + 1] / 255.0f,
                             row[x * 4 + 0] / 255.0f,
                             row[x * 4 + 3] / 255.0f});
    }

    return image;
}
} // namespace

Graphics::Image toImage(const VideoFrame& frame)
{
    if (!frame.isValid() || frame.width() <= 0 || frame.height() <= 0)
        return {};

    if (frame.pixels() != nullptr)
        return frame.format() == FramePixelFormat::NV12 ? nv12ToImage(frame)
                                                        : bgraToImage(frame);

    return nativeBufferToImage(frame.nativeBuffer());
}
} // namespace eacp::Video

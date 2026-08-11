#pragma once

#include "VideoRecorder.h"

#include <eacp/Graphics/Image/Image.h>

#include <cstddef>
#include <cstdint>

namespace eacp::Graphics
{
class View;
}

namespace eacp::Video
{

// Composites a straight-RGBA Image over black into opaque, premultiplied BGRA.
// dstStride may exceed width * 4; the image must be at least width x height.
inline void compositeOverBlackBGRA(const Graphics::Image& image,
                                   std::uint8_t* dst,
                                   int width,
                                   int height,
                                   std::size_t dstStride)
{
    const auto* src = image.pixels().data();
    auto srcStride = static_cast<std::size_t>(image.width()) * 4;

    for (auto y = 0; y < height; ++y)
    {
        const auto* s = src + static_cast<std::size_t>(y) * srcStride;
        auto* d = dst + static_cast<std::size_t>(y) * dstStride;

        for (auto x = 0; x < width; ++x)
        {
            auto r = s[x * 4 + 0];
            auto g = s[x * 4 + 1];
            auto b = s[x * 4 + 2];
            auto a = s[x * 4 + 3];

            auto overBlack = [&](std::uint8_t c) -> std::uint8_t
            { return static_cast<std::uint8_t>((c * a + 127) / 255); };

            d[x * 4 + 0] = overBlack(b);
            d[x * 4 + 1] = overBlack(g);
            d[x * 4 + 2] = overBlack(r);
            d[x * 4 + 3] = 255;
        }
    }
}

// H.264 encoder: AVFoundation on Apple, Media Foundation on Windows.
struct Encoder
{
    virtual ~Encoder() = default;

    // Overwrites any existing file. Playback timing follows the per-frame
    // presentation timestamps; fps is only the rate the encoder expects.
    virtual bool
        begin(const FilePath& path, int width, int height, int bitrate, int fps) = 0;

    // Composites over black at ptsSeconds; the image must be at least the size
    // passed to begin(). A frame offered while the encoder is busy is DROPPED,
    // so a producer needing every frame calls waitUntilReady() first.
    virtual void appendImage(const Graphics::Image& image, double ptsSeconds) = 0;

    // Blocks until the encoder can accept another frame, or the timeout
    // expires. Returns at once on backends that queue rather than drop.
    virtual void waitUntilReady(Time::MS) {}

    // GpuDirect tier, unsupported by default. The probe runs before begin(), so
    // it takes the (already even-rounded) pixel size rather than reading it
    // back.
    virtual bool canCaptureNativeContent(Graphics::View&, float, int, int)
    {
        return false;
    }
    virtual bool appendNativeContent(Graphics::View&, float, double)
    {
        return false;
    }

    // The Async resolves on the main thread once the file is fully written,
    // immediately if nothing was opened.
    virtual Threads::Async<void> finish() = 0;
};

OwningPointer<Encoder> makeEncoder();

} // namespace eacp::Video

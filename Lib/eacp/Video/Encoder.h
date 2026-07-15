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

// Composites a straight-RGBA Image over black into an opaque, premultiplied BGRA
// byte buffer, row by row honouring dstStride (which may exceed width*4 for a
// padded target). Shared by both encoders so the snapshot tier's pixel
// conversion is written once. The image must be at least width x height.
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

// The recorder's H.264 encoder, behind one interface per platform: AVFoundation
// on Apple, Media Foundation on Windows. begin() opens the file, appendImage()
// feeds one straight-RGBA frame (snapshot tier) at a real-time presentation
// timestamp, and finish() finalizes the file asynchronously.
struct Encoder
{
    virtual ~Encoder() = default;

    // Opens `path` for an H.264 stream of the given pixel size, average bitrate
    // and nominal frame rate, overwriting any existing file. Returns false on
    // setup failure. Playback timing follows the per-frame presentation
    // timestamps; fps is the rate the encoder is told to expect.
    virtual bool
        begin(const FilePath& path, int width, int height, int bitrate, int fps) = 0;

    // Appends one straight-RGBA frame, composited over black, at ptsSeconds.
    // The image must be at least the size passed to begin().
    virtual void appendImage(const Graphics::Image& image, double ptsSeconds) = 0;

    // GpuDirect tier: whether `view` has native GPU content this encoder can
    // capture zero-copy at the given (already even-rounded) pixel size, and
    // appending one such frame straight from the GPU. Both default to
    // unsupported (the snapshot/screen tiers do not use them). The probe runs
    // before begin(), so it takes the size rather than reading it back.
    virtual bool canCaptureNativeContent(Graphics::View&, float, int, int)
    {
        return false;
    }
    virtual bool appendNativeContent(Graphics::View&, float, double)
    {
        return false;
    }

    // Finalizes the file. The returned Async resolves on the main thread once it
    // is fully written (immediately if nothing was opened).
    virtual Threads::Async<void> finish() = 0;
};

// Builds the platform encoder (AVFoundation / Media Foundation).
OwningPointer<Encoder> makeEncoder();

} // namespace eacp::Video

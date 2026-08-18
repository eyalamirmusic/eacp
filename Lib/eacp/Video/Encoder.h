#pragma once

#include "Audio.h"

#include <eacp/Core/Threads/Async.h>
#include <eacp/Graphics/Image/Image.h>

#include <cstddef>
#include <cstdint>
#include <optional>

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

// The pixel side of one output file, resolved: sizes are the exact ones the
// stream will carry, not the request the caller made.
struct VideoSpec
{
    int width = 0;
    int height = 0;

    // Average H.264 bitrate in bits per second.
    int bitrate = 0;

    // The rate the encoder is told to expect; playback timing follows the
    // per-frame presentation timestamps regardless.
    int fps = 60;
};

// One file: a video track, and an audio track when there is sound to write.
// Both are declared up front because AVAssetWriter takes no further inputs
// once it has started writing.
struct EncoderSpec
{
    VideoSpec video;
    std::optional<AudioSpec> audio;
};

// The recorder's encoder, behind one interface per platform: AVFoundation on
// Apple, Media Foundation on Windows. begin() opens the file, appendImage()
// and appendAudio() feed it at real-time presentation timestamps, and finish()
// finalizes the file asynchronously.
//
// Both append paths may be called concurrently -- video arrives on the capture
// thread, audio on the recorder's drain -- so implementations serialise
// whatever the writer needs them to.
struct Encoder
{
    virtual ~Encoder() = default;

    // Opens `path` for the given tracks, overwriting any existing file.
    // Returns false on setup failure, including an audio spec the platform
    // encoder cannot honour.
    virtual bool begin(const FilePath& path, const EncoderSpec& spec) = 0;

    // Appends one straight-RGBA frame, composited over black, at ptsSeconds.
    // The image must be at least the size passed to begin().
    //
    // A frame offered while the encoder is still busy is DROPPED, not queued.
    // That is deliberate for the recorder, whose alternative is stalling the
    // display link, but it means an offline producer that needs every frame in
    // the file must call waitUntilReady() first.
    virtual void appendImage(const Graphics::Image& image, double ptsSeconds) = 0;

    // Appends one block of planar audio starting at ptsSeconds. Unlike a
    // dropped frame, a dropped block is a hole in the sound, so this waits for
    // the encoder to catch up rather than discarding it.
    virtual void appendAudio(const AudioBuffer& buffer, double ptsSeconds) = 0;

    // Whether there is an open audio track to append to. False before begin(),
    // which the screen tier only reaches once the system has handed it a
    // window -- so audio pushed in the meantime waits rather than being drained
    // into nothing.
    virtual bool acceptsAudio() const = 0;

    // Blocks until the encoder can accept another frame, or the timeout
    // expires. The recorder never calls this -- dropping late frames is the
    // behaviour it wants -- but a writer producing a file offline has no frames
    // to spare and waits here instead. Defaults to returning at once, for
    // backends that queue rather than drop.
    virtual void waitUntilReady(Time::MS) {}

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

#pragma once

#include <eacp/Core/Core.h>

namespace eacp::Video
{
enum class FramePixelFormat
{
    BGRA8,

    // Two planes in one buffer: `height` rows of 8-bit luma, then `height / 2`
    // rows of interleaved Cb/Cr at half resolution on both axes. Both planes
    // share `bytesPerRow`, so the chroma plane starts at bytesPerRow * height.
    NV12
};

// Bytes one frame of this size and format occupies, tightly packed.
constexpr std::size_t framePixelBytes(FramePixelFormat format, int width, int height)
{
    auto pixels = (std::size_t) width * (std::size_t) height;
    return format == FramePixelFormat::NV12 ? pixels + pixels / 2 : pixels * 4;
}

// Which YCbCr matrix a track's chroma was coded with; decoding one as the other
// is a visible error.
enum class YuvMatrix
{
    BT601,
    BT709
};

// Turns NV12 samples into RGB, with y/u/v the raw 0-1 samples after offset and
// scale: R = y + redV * v, G = y - greenU * u - greenV * v, B = y + blueU * u.
struct YuvTransform
{
    float lumaOffset = 0.0f;
    float lumaScale = 1.0f;
    float chromaOffset = 0.0f;
    float chromaScale = 1.0f;

    float redV = 0.0f;
    float greenU = 0.0f;
    float greenV = 0.0f;
    float blueU = 0.0f;
};

// `fullRange` selects between video levels (luma 16-235, chroma 16-240) and the
// full 0-255 that JPEG-style and some camera sources use.
constexpr YuvTransform yuvTransformFor(YuvMatrix matrix, bool fullRange)
{
    auto kr = matrix == YuvMatrix::BT709 ? 0.2126f : 0.299f;
    auto kb = matrix == YuvMatrix::BT709 ? 0.0722f : 0.114f;
    auto kg = 1.0f - kr - kb;

    auto transform = YuvTransform {};

    transform.lumaOffset = fullRange ? 0.0f : 16.0f / 255.0f;
    transform.lumaScale = fullRange ? 1.0f : 255.0f / 219.0f;
    transform.chromaOffset = 128.0f / 255.0f;
    transform.chromaScale = fullRange ? 1.0f : 255.0f / 224.0f;

    transform.redV = 2.0f * (1.0f - kr);
    transform.greenU = 2.0f * kb * (1.0f - kb) / kg;
    transform.greenV = 2.0f * kr * (1.0f - kr) / kg;
    transform.blueU = 2.0f * (1.0f - kb);

    return transform;
}

// What to assume when a track does not signal its matrix. 576 is the tallest
// standard-definition frame, which BT.601 was written for.
constexpr YuvMatrix yuvMatrixForHeight(int height)
{
    return height > 576 ? YuvMatrix::BT709 : YuvMatrix::BT601;
}

struct FrameInfo
{
    int width = 0;
    int height = 0;

    // Presentation time and on-screen duration, in seconds from the start of
    // the file. Duration 0 means the container did not say.
    double seconds = 0.0;
    double duration = 0.0;

    // May exceed width * 4 on a padded buffer. CPU path only; the zero-copy
    // path reads it from the platform buffer.
    std::size_t bytesPerRow = 0;

    FramePixelFormat format = FramePixelFormat::BGRA8;

    // How to read this frame's chroma. Only meaningful for NV12.
    YuvMatrix yuvMatrix = YuvMatrix::BT709;
    bool fullRangeYuv = false;
};

// A timestamp plus pixels, either as a platform buffer the GPU can wrap without
// copying or as a CPU-side copy. Owns its pixels and is reference counted, so
// copies are cheap and share the same pixels.
class VideoFrame
{
public:
    using Releaser = std::function<void(void*)>;

    VideoFrame() = default;

    // Takes ownership of a platform pixel buffer with the backend's retain
    // already applied; `release` runs once the last sharing frame is destroyed.
    static VideoFrame
        fromNativeBuffer(void* buffer, Releaser release, const FrameInfo& info)
    {
        auto frame = VideoFrame {};
        frame.payload = std::make_shared<Payload>(buffer, std::move(release), info);
        return frame;
    }

    // Takes ownership of a CPU-side copy, for backends without a GPU-wrappable
    // buffer.
    static VideoFrame fromPixels(Vector<std::uint8_t> pixels, const FrameInfo& info)
    {
        return fromPixelBuffer(
            std::make_shared<Vector<std::uint8_t>>(std::move(pixels)), info);
    }

    // Shares a recycled buffer instead of handing over a fresh one. It must not
    // be written again until every VideoFrame sharing it is gone, which a
    // backend checks with use_count().
    static VideoFrame fromPixelBuffer(std::shared_ptr<Vector<std::uint8_t>> pixels,
                                      const FrameInfo& info)
    {
        auto frame = VideoFrame {};
        frame.payload = std::make_shared<Payload>(std::move(pixels), info);
        return frame;
    }

    bool isValid() const { return payload != nullptr; }

    const FrameInfo& info() const
    {
        static const auto empty = FrameInfo {};
        return payload != nullptr ? payload->info : empty;
    }

    int width() const { return info().width; }
    int height() const { return info().height; }
    double seconds() const { return info().seconds; }
    double duration() const { return info().duration; }
    std::size_t bytesPerRow() const { return info().bytesPerRow; }
    FramePixelFormat format() const { return info().format; }

    YuvTransform yuvTransform() const
    {
        return yuvTransformFor(info().yuvMatrix, info().fullRangeYuv);
    }

    // A frame with no duration covers everything from its timestamp on, so the
    // last frame of a stream keeps being shown.
    bool covers(double time) const
    {
        if (!isValid() || time < seconds())
            return false;

        return duration() <= 0.0 || time < seconds() + duration();
    }

    // For a zero-copy GPU wrap; null when this frame carries CPU pixels. Valid
    // while this VideoFrame or any copy of it is alive.
    void* nativeBuffer() const
    {
        return payload != nullptr ? payload->buffer : nullptr;
    }

    // Null on the zero-copy path.
    const std::uint8_t* pixels() const
    {
        if (payload == nullptr || payload->pixels == nullptr
            || payload->pixels->size() == 0)
            return nullptr;

        return payload->pixels->data();
    }

    // Null for any format but NV12. Shares bytesPerRow() with the luma plane
    // and has half as many rows.
    const std::uint8_t* chromaPlane() const
    {
        if (format() != FramePixelFormat::NV12)
            return nullptr;

        const auto* base = pixels();

        if (base == nullptr)
            return nullptr;

        return base + bytesPerRow() * (std::size_t) height();
    }

private:
    struct Payload
    {
        Payload(void* bufferToUse, Releaser releaseToUse, const FrameInfo& infoToUse)
            : info(infoToUse)
            , buffer(bufferToUse)
            , release(std::move(releaseToUse))
        {
        }

        Payload(std::shared_ptr<Vector<std::uint8_t>> pixelsToUse,
                const FrameInfo& infoToUse)
            : info(infoToUse)
            , pixels(std::move(pixelsToUse))
        {
        }

        ~Payload()
        {
            if (buffer != nullptr)
                release(buffer);
        }

        Payload(const Payload&) = delete;
        Payload& operator=(const Payload&) = delete;

        FrameInfo info;
        void* buffer = nullptr;
        Releaser release = [](void*) {};
        std::shared_ptr<Vector<std::uint8_t>> pixels;
    };

    std::shared_ptr<const Payload> payload;
};
} // namespace eacp::Video

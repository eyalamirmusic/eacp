#pragma once

#include <eacp/Core/Core.h>

namespace eacp::Video
{
// The pixel layout a decoded frame arrives in. The Windows backend hands back
// NV12, straight from the decoder; the Apple zero-copy path still wraps a BGRA
// CVPixelBuffer.
enum class FramePixelFormat
{
    BGRA8,

    // Two planes in one buffer: `height` rows of 8-bit luma, then `height / 2`
    // rows of interleaved Cb/Cr at half resolution on both axes. Both planes
    // share `bytesPerRow`, so the chroma plane starts at bytesPerRow * height.
    //
    // What every video decoder produces natively. Taking it as-is rather than
    // asking the platform for BGRA skips a colour-conversion pass over every
    // frame and carries 1.5 bytes per pixel instead of 4 — at 8K that is 50 MB
    // a frame rather than 133 MB, through both the copy and the upload. The
    // conversion happens in the shader, where it is free.
    NV12
};

// Bytes one frame of this size and format occupies, tightly packed.
constexpr std::size_t framePixelBytes(FramePixelFormat format, int width, int height)
{
    auto pixels = (std::size_t) width * (std::size_t) height;
    return format == FramePixelFormat::NV12 ? pixels + pixels / 2 : pixels * 4;
}

// Which YCbCr matrix a track's chroma was coded with. Not a detail that can be
// assumed: BT.601 was defined for standard definition and BT.709 for high, and
// decoding one as the other is a visible error — a saturated green comes back
// about 15% dark, which is enough to fail a round-trip comparison.
enum class YuvMatrix
{
    BT601,
    BT709
};

// The constants that turn NV12 samples into RGB. Derived from the matrix rather
// than written out, so the CPU path in toImage and the shader in SpriteRenderer
// are provably the same arithmetic.
//
//     R = y + redV * v
//     G = y - greenU * u - greenV * v
//     B = y + blueU * u
//
// where y, u and v are the raw 0-1 samples with the offsets subtracted and the
// scales applied.
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
    // The red and blue luma weights each matrix is defined by; everything else
    // follows from them.
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

// What to assume when a track does not signal its matrix, which is the common
// case: the definition each standard was written for. 576 is the tallest
// standard-definition frame.
constexpr YuvMatrix yuvMatrixForHeight(int height)
{
    return height > 576 ? YuvMatrix::BT709 : YuvMatrix::BT601;
}

// Everything about a decoded frame except its pixels.
struct FrameInfo
{
    int width = 0;
    int height = 0;

    // Presentation time and on-screen duration, in seconds from the start of
    // the file. A duration of 0 means the container did not say; FrameStream
    // then falls back to the track's nominal frame rate.
    double seconds = 0.0;
    double duration = 0.0;

    // Distance between rows, which may exceed width * 4 on a padded buffer.
    // Only meaningful for the CPU path; the zero-copy path reads it from the
    // platform buffer.
    std::size_t bytesPerRow = 0;

    FramePixelFormat format = FramePixelFormat::BGRA8;

    // How to read this frame's chroma. Only meaningful for NV12.
    YuvMatrix yuvMatrix = YuvMatrix::BT709;
    bool fullRangeYuv = false;
};

// One decoded frame: a timestamp plus pixels, either as a platform buffer the
// GPU can wrap without copying (a CVPixelBuffer on Apple) or as a CPU-side
// BGRA8 copy.
//
// Unlike Cameras::CameraFrame — a non-owning view valid only inside the capture
// callback — this owns its pixels and is reference counted, because a video
// stream keeps several frames alive at once: the decoder runs ahead of the
// playhead, and the renderer holds the one it is drawing for the length of a
// render pass while the decode thread carries on filling the queue behind it.
// Copies are cheap and share the same pixels.
//
// The platform's release call reaches this class as a std::function handed over
// at construction, so the frame owns a native buffer without the portable layer
// naming a platform type.
class VideoFrame
{
public:
    using Releaser = std::function<void(void*)>;

    VideoFrame() = default;

    // Takes ownership of a platform pixel buffer; `release` runs once the last
    // VideoFrame sharing it is destroyed. The backend passes its own retain
    // already applied (CFRetain on Apple) and CFRelease as the releaser.
    static VideoFrame
        fromNativeBuffer(void* buffer, Releaser release, const FrameInfo& info)
    {
        auto frame = VideoFrame {};
        frame.payload = std::make_shared<Payload>(buffer, std::move(release), info);
        return frame;
    }

    // Takes ownership of a CPU-side BGRA8 copy, for backends without a
    // GPU-wrappable buffer (Media Foundation today).
    static VideoFrame fromPixels(Vector<std::uint8_t> pixels, const FrameInfo& info)
    {
        return fromPixelBuffer(
            std::make_shared<Vector<std::uint8_t>>(std::move(pixels)), info);
    }

    // Shares an existing pixel buffer rather than handing over a fresh one, so a
    // backend can recycle buffers across frames instead of allocating one per
    // frame. At 4K that allocation is 33 MB and at 8K 133 MB, and a fresh one
    // costs a page fault and a kernel zero-fill for every page before the decoder
    // even starts copying into it.
    //
    // The buffer must not be written again until every VideoFrame sharing it is
    // gone; a backend checks that with use_count() before reusing one.
    static VideoFrame fromPixelBuffer(std::shared_ptr<Vector<std::uint8_t>> pixels,
                                      const FrameInfo& info)
    {
        auto frame = VideoFrame {};
        frame.payload = std::make_shared<Payload>(std::move(pixels), info);
        return frame;
    }

    // Points at pixels inside a buffer belonging to something else — a decoder
    // output sample the backend has locked open — rather than a buffer of ours.
    // `owner` is whatever keeps them readable, unlocked and handed back when
    // the last frame sharing them is destroyed.
    //
    // This is the copy fromPixelBuffer cannot avoid. A backend can hold one of
    // its decoder's buffers per queued frame instead of draining each into a
    // buffer of its own, which takes a full-frame copy off the decode thread —
    // at 8K, 50 MB read and 50 MB written per frame.
    //
    // The cost is holding that many of the decoder's output buffers, so it is
    // only safe while the frames alive at once stay few, which FrameStream's
    // queue depth is what bounds.
    static VideoFrame fromBorrowedPixels(const std::uint8_t* pixels,
                                         std::shared_ptr<void> owner,
                                         const FrameInfo& info)
    {
        auto frame = VideoFrame {};
        frame.payload = std::make_shared<Payload>(pixels, std::move(owner), info);
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

    // Whether `time` falls in this frame's presentation interval. A frame with
    // no duration covers everything from its own timestamp on, so the last
    // frame of a stream keeps being shown rather than blinking out.
    bool covers(double time) const
    {
        if (!isValid() || time < seconds())
            return false;

        return duration() <= 0.0 || time < seconds() + duration();
    }

    // The platform pixel buffer for a zero-copy GPU wrap, or null when this
    // frame carries CPU pixels instead. Valid for as long as this VideoFrame
    // (or any copy of it) is alive.
    void* nativeBuffer() const
    {
        return payload != nullptr ? payload->buffer : nullptr;
    }

    // The CPU-side pixels, or null on the zero-copy path.
    const std::uint8_t* pixels() const
    {
        return payload != nullptr ? payload->pixelData : nullptr;
    }

    // The interleaved Cb/Cr plane of an NV12 frame, or null for any other
    // format. It shares bytesPerRow() with the luma plane and has half as many
    // rows, each covering two pixels' worth of chroma.
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
            , pixelData(pixels != nullptr && pixels->size() > 0 ? pixels->data()
                                                                : nullptr)
        {
        }

        Payload(const std::uint8_t* pixelsToUse,
                std::shared_ptr<void> ownerToUse,
                const FrameInfo& infoToUse)
            : info(infoToUse)
            , owner(std::move(ownerToUse))
            , pixelData(pixelsToUse)
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

        // A platform pixel buffer the GPU can wrap, on the zero-copy path only.
        void* buffer = nullptr;
        Releaser release = [](void*) {};

        // Pixels this frame owns, and pixels it only borrows. Exactly one of
        // these carries the memory `pixelData` points into, and both keep it
        // alive for as long as the payload.
        std::shared_ptr<Vector<std::uint8_t>> pixels;
        std::shared_ptr<void> owner;

        const std::uint8_t* pixelData = nullptr;
    };

    std::shared_ptr<const Payload> payload;
};
} // namespace eacp::Video

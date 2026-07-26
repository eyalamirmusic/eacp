#pragma once

#include <eacp/Core/Core.h>

namespace eacp::Video
{
// The pixel layout a decoded frame arrives in. Every backend is asked for BGRA8
// today, which is what GPU::TextureFormat::BGRA8Unorm and the zero-copy
// CVPixelBuffer wrap already understand; NV12 lands once the GPU module grows a
// two-channel format and a YUV sampling program.
enum class FramePixelFormat
{
    BGRA8
};

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

    // The CPU-side BGRA8 pixels, or null on the zero-copy path.
    const std::uint8_t* pixels() const
    {
        if (payload == nullptr || payload->pixels == nullptr
            || payload->pixels->size() == 0)
            return nullptr;

        return payload->pixels->data();
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

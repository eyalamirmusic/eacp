#include "VideoFrame.h"

namespace eacp::Video
{

struct VideoFrame::Payload
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

VideoFrame VideoFrame::fromNativeBuffer(void* buffer,
                                        Releaser release,
                                        const FrameInfo& info)
{
    auto frame = VideoFrame {};
    frame.payload = std::make_shared<Payload>(buffer, std::move(release), info);
    return frame;
}

VideoFrame VideoFrame::fromPixels(Vector<std::uint8_t> pixels, const FrameInfo& info)
{
    return fromPixelBuffer(std::make_shared<Vector<std::uint8_t>>(std::move(pixels)),
                           info);
}

VideoFrame VideoFrame::fromPixelBuffer(std::shared_ptr<Vector<std::uint8_t>> pixels,
                                       const FrameInfo& info)
{
    auto frame = VideoFrame {};
    frame.payload = std::make_shared<Payload>(std::move(pixels), info);
    return frame;
}

bool VideoFrame::isValid() const
{
    return payload != nullptr;
}

const FrameInfo& VideoFrame::info() const
{
    static const auto empty = FrameInfo {};
    return payload != nullptr ? payload->info : empty;
}

int VideoFrame::width() const
{
    return info().width;
}

int VideoFrame::height() const
{
    return info().height;
}

double VideoFrame::seconds() const
{
    return info().seconds;
}

double VideoFrame::duration() const
{
    return info().duration;
}

int VideoFrame::bytesPerRow() const
{
    return info().bytesPerRow;
}

FramePixelFormat VideoFrame::format() const
{
    return info().format;
}

YuvTransform VideoFrame::yuvTransform() const
{
    return yuvTransformFor(info().yuvMatrix, info().fullRangeYuv);
}

bool VideoFrame::covers(double time) const
{
    if (!isValid() || time < seconds())
        return false;

    return duration() <= 0.0 || time < seconds() + duration();
}

void* VideoFrame::nativeBuffer() const
{
    return payload != nullptr ? payload->buffer : nullptr;
}

const std::uint8_t* VideoFrame::pixels() const
{
    if (payload == nullptr || payload->pixels == nullptr
        || payload->pixels->size() == 0)
        return nullptr;

    return payload->pixels->data();
}

const std::uint8_t* VideoFrame::chromaPlane() const
{
    if (format() != FramePixelFormat::NV12)
        return nullptr;

    const auto* base = pixels();

    if (base == nullptr)
        return nullptr;

    return base + bytesPerRow() * height();
}

} // namespace eacp::Video

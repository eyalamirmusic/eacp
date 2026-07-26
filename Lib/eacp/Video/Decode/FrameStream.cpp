#include "FrameStream.h"

#include <algorithm>
#include <chrono>

namespace eacp::Video
{
FrameStream::FrameStream() = default;

FrameStream::~FrameStream()
{
    close();
}

bool FrameStream::open(const FilePath& path, const StreamOptions& options)
{
    auto platformDecoder = makeDecoder();

    if (!platformDecoder->open(path))
        return false;

    return open(std::move(platformDecoder), options);
}

bool FrameStream::open(OwningPointer<Decoder> decoderToUse,
                       const StreamOptions& options)
{
    close();

    if (decoderToUse == nullptr)
        return false;

    decoder = std::move(decoderToUse);
    videoInfo = decoder->info();
    queueDepth = depthWithinBudget(videoInfo, options);

    startDecodeThread();
    return true;
}

int FrameStream::depthWithinBudget(const VideoInfo& info,
                                   const StreamOptions& options)
{
    auto requested = std::max(1, options.queueDepth);
    auto frameBytes = (std::size_t) std::max(0, info.width)
                      * (std::size_t) std::max(0, info.height) * 4u;

    if (frameBytes == 0 || options.maxQueueBytes == 0)
        return requested;

    auto affordable = (int) (options.maxQueueBytes / frameBytes);
    return std::clamp(affordable, 1, requested);
}

void FrameStream::close()
{
    stopDecodeThread();

    decoder = nullptr;
    videoInfo = {};
    current = {};
    queue.clear();
    endOfStream = false;
    pendingSeek.reset();
    framesDecoded = 0;
    framesSkipped = 0;
}

bool FrameStream::isOpen() const
{
    return decoder != nullptr;
}

void FrameStream::setFrameReadyCallback(Callback callback)
{
    auto lock = std::lock_guard {callbackMutex};
    frameReady = callback ? std::move(callback) : Callback {[] {}};
}

void FrameStream::startDecodeThread()
{
    stopping = false;
    decodeThread = std::thread {[this] { decodeLoop(); }};
}

void FrameStream::stopDecodeThread()
{
    if (!decodeThread.joinable())
        return;

    {
        auto lock = std::lock_guard {mutex};
        stopping = true;
    }

    spaceAvailable.notify_all();
    decodeThread.join();
}

void FrameStream::decodeLoop()
{
    while (true)
    {
        auto seekTo = std::optional<double> {};
        auto seekMode = SeekMode::Accurate;

        {
            auto lock = std::unique_lock {mutex};

            spaceAvailable.wait(lock,
                                [this]
                                {
                                    return stopping || pendingSeek.has_value()
                                           || (!endOfStream
                                               && (int) queue.size() < queueDepth);
                                });

            if (stopping)
                return;

            if (pendingSeek.has_value())
            {
                seekTo = pendingSeek;
                seekMode = pendingSeekMode;
                pendingSeek.reset();

                // Everything queued is from before the jump.
                queue.clear();
                endOfStream = false;
            }
        }

        // The decoder is only ever touched from this thread, so both calls run
        // outside the lock: seeking and decoding hit the disk, and holding the
        // lock across either would stall every frameAt() on the render thread.
        if (seekTo.has_value())
        {
            decoder->seek(*seekTo, seekMode);
            continue;
        }

        auto frame = VideoFrame {};
        auto decoded = decoder->nextFrame(frame);

        {
            auto lock = std::lock_guard {mutex};

            // A seek arrived while this frame was decoding: it belongs to the
            // old position, so drop it and let the loop top handle the seek.
            if (pendingSeek.has_value())
                continue;

            if (decoded)
            {
                queue.push_back(std::move(frame));
                ++framesDecoded;
            }
            else
            {
                endOfStream = true;
            }
        }

        frameAvailable.notify_all();

        if (decoded)
        {
            auto callback = Callback {};

            {
                auto lock = std::lock_guard {callbackMutex};
                callback = frameReady;
            }

            callback();
        }
    }
}

void FrameStream::advanceTo(double seconds)
{
    auto dropped = false;

    while (!queue.empty() && queue.front().seconds() <= seconds)
    {
        // Every frame this loop passes over except the last is one that was
        // decoded and then never shown. `dropped` being set means the frame
        // about to be replaced was itself taken by this same call.
        if (dropped)
            ++framesSkipped;

        current = std::move(queue.front());
        queue.pop_front();
        dropped = true;
    }

    // Waking the decode thread belongs here rather than at the call sites,
    // because waitForFrameAt runs this from inside its condition predicate: a
    // wait that needs more frames than the queue holds would otherwise drain
    // the queue and then block against a producer still convinced it is full.
    if (dropped)
        spaceAvailable.notify_all();
}

VideoFrame FrameStream::frameAt(double seconds)
{
    auto lock = std::lock_guard {mutex};
    advanceTo(seconds);
    return current;
}

VideoFrame FrameStream::waitForFrameAt(double seconds, Time::MS timeout)
{
    auto lock = std::unique_lock {mutex};

    frameAvailable.wait_for(lock,
                            std::chrono::milliseconds {timeout.count},
                            [this, seconds]
                            {
                                advanceTo(seconds);
                                return current.covers(seconds) || atEnd();
                            });

    return current;
}

void FrameStream::seek(double seconds, SeekMode mode)
{
    {
        auto lock = std::lock_guard {mutex};
        pendingSeek = std::max(0.0, seconds);
        pendingSeekMode = mode;

        // Clearing this here rather than leaving it to the decode thread
        // matters: a caller that seeks and then immediately waits for a frame
        // would otherwise see the end-of-stream left over from before the jump
        // and give up at once, handing back the frame it was already showing.
        endOfStream = false;
    }

    spaceAvailable.notify_all();
}

bool FrameStream::atEnd() const
{
    // A pending seek has not been carried out yet, so whatever the decoder
    // reported before it says nothing about where the stream is going.
    return endOfStream && !pendingSeek.has_value() && queue.empty();
}

bool FrameStream::hasReachedEnd() const
{
    auto lock = std::lock_guard {mutex};
    return atEnd();
}

FrameStream::Stats FrameStream::stats() const
{
    auto lock = std::lock_guard {mutex};

    return {framesDecoded, framesSkipped, (int) queue.size(), queueDepth};
}
} // namespace eacp::Video

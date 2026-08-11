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
    framesStarved = 0;
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
        // outside the lock rather than stalling frameAt() on disk access.
        if (seekTo.has_value())
        {
            decoder->seek(*seekTo, seekMode);
            continue;
        }

        auto frame = VideoFrame {};
        auto decoded = decoder->nextFrame(frame);

        {
            auto lock = std::lock_guard {mutex};

            // A frame decoded across a seek belongs to the old position.
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
        // Every frame this call passes over but the last was never shown.
        if (dropped)
            ++framesSkipped;

        current = std::move(queue.front());
        queue.pop_front();
        dropped = true;
    }

    // Waking the producer belongs here, not at the call sites: waitForFrameAt
    // runs this inside its predicate and would otherwise drain the queue and
    // then block against a producer still convinced it is full.
    if (dropped)
        spaceAvailable.notify_all();
}

VideoFrame FrameStream::frameAt(double seconds)
{
    auto lock = std::lock_guard {mutex};
    advanceTo(seconds);

    // The caller is about to redraw a frame it has already drawn.
    if (!atEnd() && !current.covers(seconds))
        ++framesStarved;

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

        // Cleared here, not on the decode thread: a caller that seeks and
        // immediately waits would otherwise see the stale end-of-stream.
        endOfStream = false;
    }

    spaceAvailable.notify_all();
}

bool FrameStream::atEnd() const
{
    // What the decoder reported before a pending seek says nothing about where
    // the stream is going.
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

    return {
        framesDecoded, framesSkipped, framesStarved, (int) queue.size(), queueDepth};
}
} // namespace eacp::Video

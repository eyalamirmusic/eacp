#pragma once

#include "Decoder.h"

#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>

namespace eacp::Video
{
struct StreamOptions
{
    // How many decoded frames may wait ahead of the playhead. The decode thread
    // blocks once the queue is this deep, which is the backpressure.
    int queueDepth = 4;

    // Ceiling on the queue's memory; open() lowers queueDepth until it fits,
    // never below one. Budget one frame more: the frame being shown is held
    // outside the queue. The default keeps the full depth at 8K.
    std::size_t maxQueueBytes = 512u * 1024u * 1024u;
};

// A Decoder, the thread that runs it ahead of the consumer, and the bounded
// queue between them. frameAt() only moves forwards through that queue, so a
// playhead that jumps backwards needs seek() first.
class FrameStream
{
public:
    FrameStream();
    ~FrameStream();

    FrameStream(const FrameStream&) = delete;
    FrameStream& operator=(const FrameStream&) = delete;

    // Opens `path` with the platform decoder and starts decoding ahead.
    bool open(const FilePath& path, const StreamOptions& options = {});

    // Takes an already-opened decoder instead, e.g. a synthetic or non-platform
    // source.
    bool open(OwningPointer<Decoder> decoderToUse,
              const StreamOptions& options = {});

    void close();
    bool isOpen() const;

    const VideoInfo& info() const { return videoInfo; }

    // The newest queued frame that has started by `seconds`; frames behind the
    // playhead are dropped. Never blocks, and once anything has been decoded
    // never returns an invalid frame — it repeats the last one handed out.
    VideoFrame frameAt(double seconds);

    // Blocks until a frame covering `seconds` is decoded, then as frameAt().
    // Returns the best frame available if the timeout expires.
    VideoFrame waitForFrameAt(double seconds, Time::MS timeout);

    // Returns at once: the decode thread performs the seek and refills the
    // queue, and frameAt() keeps returning the previous frame until it does.
    void seek(double seconds, SeekMode mode = SeekMode::Accurate);

    // Whether the decoder ran out of frames *and* the queue has drained.
    bool hasReachedEnd() const;

    struct Stats
    {
        // Decode work done since open, not reset by seeking.
        std::uint64_t decoded = 0;

        // Frames the playhead passed without them ever being drawn; those a
        // seek threw away do not count. Rising means presentation is behind.
        std::uint64_t skipped = 0;

        // Times frameAt() had to repeat the previous frame. Rising means decode
        // is the bottleneck. The one to watch at high resolutions, where the
        // memory budget leaves a queue of one and `skipped` can never fire.
        std::uint64_t starved = 0;

        int queued = 0;
        int depth = 0;
    };

    Stats stats() const;

    // The requested depth, lowered until maxQueueBytes covers that many frames,
    // never below one. open() applies it.
    static int depthWithinBudget(const VideoInfo& info,
                                 const StreamOptions& options);

    // Fired on the decode thread each time a frame lands in the queue. Keep it
    // short and marshal to the main thread; passing {} clears it.
    void setFrameReadyCallback(Callback callback);

private:
    void startDecodeThread();
    void stopDecodeThread();
    void decodeLoop();

    // Both called with `mutex` held.
    void advanceTo(double seconds);
    bool atEnd() const;

    OwningPointer<Decoder> decoder;
    VideoInfo videoInfo;
    int queueDepth = 4;

    // The frame most recently handed to a consumer, and what frameAt() falls
    // back to.
    VideoFrame current;

    std::deque<VideoFrame> queue;
    mutable std::mutex mutex;
    std::condition_variable spaceAvailable; // decode thread waits on this
    std::condition_variable frameAvailable; // waitForFrameAt waits on this

    std::uint64_t framesDecoded = 0;
    std::uint64_t framesSkipped = 0;
    std::uint64_t framesStarved = 0;

    bool stopping = false;
    bool endOfStream = false;
    std::optional<double> pendingSeek;
    SeekMode pendingSeekMode = SeekMode::Accurate;

    std::mutex callbackMutex;
    Callback frameReady = [] {};

    std::thread decodeThread;
};
} // namespace eacp::Video

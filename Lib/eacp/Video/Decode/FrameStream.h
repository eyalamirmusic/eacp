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
    // blocks once the queue is this deep, which is the backpressure: a stream
    // nobody is drawing costs one frame of work and then nothing. Deeper
    // absorbs more decode jitter at the cost of that many frames of memory —
    // roughly width * height * 4 bytes each.
    int queueDepth = 4;
};

// A decoded-frame stream: a Decoder, the thread that runs it ahead of the
// consumer, and the bounded queue of frames waiting to be shown.
//
// This is where decoding and presentation come apart. Nothing is ever pushed at
// the consumer; the consumer asks for the frame that belongs at a moment:
//
//     auto frame = stream.frameAt(myTime);
//
// which is what lets one object serve a media player (time from a playback
// clock), a game (time from the simulation, so pausing or slowing the world
// pauses or slows the video with it) and an editor (time from the timeline
// playhead). The frames themselves are ref-counted and outlive the queue, so a
// consumer can hold on to one, or composite several streams into one pass,
// without the decoder having to know.
//
// frameAt() only ever moves forwards through the queue: a playhead that jumps
// backwards needs seek() first, which Player::setPosition does for you.
class FrameStream
{
public:
    FrameStream();
    ~FrameStream();

    FrameStream(const FrameStream&) = delete;
    FrameStream& operator=(const FrameStream&) = delete;

    // Opens `path` with the platform decoder and starts decoding ahead.
    bool open(const FilePath& path, const StreamOptions& options = {});

    // Takes an already-opened decoder instead — a synthetic one in tests, or a
    // non-platform source (an image sequence, a network stream) in an app. The
    // whole timing and queueing layer above is portable C++, so this is all it
    // takes to exercise it with no media and no OS involved.
    bool open(OwningPointer<Decoder> decoderToUse,
              const StreamOptions& options = {});

    void close();
    bool isOpen() const;

    const VideoInfo& info() const { return videoInfo; }

    // The frame that belongs on screen at `seconds`: the newest queued frame
    // that has already started by then. Frames older than that are dropped —
    // they are behind the playhead and will never be shown, which is how a
    // stream catches up after a stall.
    //
    // Never blocks and never returns an invalid frame once one has been
    // decoded: if nothing new is ready it returns the frame it last handed out,
    // so a renderer always has something to draw.
    VideoFrame frameAt(double seconds);

    // As above, but waits for a frame covering `seconds` to be decoded. For
    // callers that must not miss a frame rather than stay responsive: an
    // offline render, an editor stepping frame by frame, a test. Returns the
    // best frame available if the timeout expires.
    VideoFrame waitForFrameAt(double seconds, Time::MS timeout);

    // Repositions the stream. Returns at once — the decode thread performs the
    // seek and refills the queue, and frameAt() keeps returning the previous
    // frame until it does.
    void seek(double seconds, SeekMode mode = SeekMode::Accurate);

    // Whether the decoder has run out of frames *and* the queue has drained,
    // i.e. the last frame of the file is the one now being shown.
    bool hasReachedEnd() const;

    // What the stream has actually been doing, for a HUD or a benchmark.
    struct Stats
    {
        // Frames the decoder produced since the stream was opened. Seeking does
        // not reset it: it is a measure of decode work done, not of position.
        std::uint64_t decoded = 0;

        // Frames the playhead passed without them ever being drawn. A steadily
        // rising count means decode is keeping up but presentation is not —
        // the renderer is behind, or the playhead is moving faster than real
        // time. Frames thrown away by a seek are not counted; they were never
        // going to be shown.
        std::uint64_t skipped = 0;

        // How full the queue is right now, against the depth it was given.
        // Sitting at zero means the decoder is the bottleneck.
        int queued = 0;
        int depth = 0;
    };

    Stats stats() const;

    // Fired on the decode thread each time a frame lands in the queue — the
    // hook a view uses to render the moment new content exists rather than
    // waiting to be noticed at the next display tick (see
    // Cameras::Camera::setFrameArrivedCallback, which plays the same role).
    // Keep it short and marshal to the main thread. Passing {} clears it.
    void setFrameReadyCallback(Callback callback);

private:
    void startDecodeThread();
    void stopDecodeThread();
    void decodeLoop();

    // Advances `current` to the newest queued frame that has started by
    // `seconds`, discarding the ones it passes. Called with `mutex` held.
    void advanceTo(double seconds);

    // Whether the stream really has nothing more to show. Called with `mutex`
    // held.
    bool atEnd() const;

    OwningPointer<Decoder> decoder;
    VideoInfo videoInfo;
    int queueDepth = 4;

    // The frame most recently handed to a consumer — what frameAt() falls back
    // to, and what keeps the last frame of a file on screen at the end.
    VideoFrame current;

    std::deque<VideoFrame> queue;
    mutable std::mutex mutex;
    std::condition_variable spaceAvailable; // decode thread waits on this
    std::condition_variable frameAvailable; // waitForFrameAt waits on this

    std::uint64_t framesDecoded = 0;
    std::uint64_t framesSkipped = 0;

    bool stopping = false;
    bool endOfStream = false;
    std::optional<double> pendingSeek;
    SeekMode pendingSeekMode = SeekMode::Accurate;

    std::mutex callbackMutex;
    Callback frameReady = [] {};

    std::thread decodeThread;
};
} // namespace eacp::Video

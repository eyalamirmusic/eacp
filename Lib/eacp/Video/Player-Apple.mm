#import <AVFoundation/AVFoundation.h>
#import <QuartzCore/QuartzCore.h>

#include "Player.h"

#include <eacp/Core/ObjC/AutoReleasePool.h>
#include <eacp/Core/ObjC/ObjC.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <mutex>
#include <optional>
#include <thread>

// macOS/iOS playback backend (AVFoundation). AVPlayer owns the clock, audio
// and A/V sync; an AVPlayerItemVideoOutput hands out BGRA CVPixelBuffers.
//
// The pull those buffers need is driven by a pacing thread this backend owns,
// not by whoever happens to be rendering. That makes playback the same shape
// as capture: Camera has AVFoundation's capture queue publishing each frame as
// the latest and announcing it, and here the pacing thread does exactly that,
// as Player-Windows' decode thread already did. Two things follow. Decoding
// never happens on the render thread, so a slow frame cannot stall the
// compositor. And a view driven by the announcement redraws at the clip's
// frame rate — a 24fps clip on a 120Hz display renders 24 times a second, not
// 120, with the other 96 no longer re-drawing a frame that never changed.
//
// MRC throughout — every alloc/init is owned by an ObjC::Ptr.

namespace eacp::Video
{
namespace
{
// Thread-safe holder for the most recent frame's pixel buffer: the pull
// refreshes it, the render thread acquires it. Each access is a tiny critical
// section guarding a retained CVPixelBuffer.
struct LatestFrame
{
    ~LatestFrame() { clear(); }

    void clear()
    {
        std::lock_guard<std::mutex> lock(mutex);

        if (current != nullptr)
            CFRelease(current);

        current = nullptr;

        // Reset too, or a reopened player reports frames a consumer has
        // already seen and its first real frame is skipped as stale.
        sequence = 0;
    }

    void set(CVPixelBufferRef buffer)
    {
        auto* retained = (CVPixelBufferRef) CFRetain(buffer);
        CVPixelBufferRef previous = nullptr;

        {
            std::lock_guard<std::mutex> lock(mutex);
            previous = current;
            current = retained;
            ++sequence;
        }

        if (previous != nullptr)
            CFRelease(previous);
    }

    // Returns the current buffer retained (caller releases), or null.
    void* acquire()
    {
        std::lock_guard<std::mutex> lock(mutex);

        if (current != nullptr)
            CFRetain(current);

        return current;
    }

    std::uint64_t currentSequence()
    {
        std::lock_guard<std::mutex> lock(mutex);
        return sequence;
    }

    // Copies the current frame into `out` as tightly packed BGRA when it is
    // newer than out.sequence.
    bool copyInto(FramePixels& out)
    {
        CVPixelBufferRef buffer = nullptr;
        std::uint64_t copiedSequence = 0;

        {
            std::lock_guard<std::mutex> lock(mutex);

            if (current == nullptr || sequence == out.sequence)
                return false;

            buffer = (CVPixelBufferRef) CFRetain(current);
            copiedSequence = sequence;
        }

        CVPixelBufferLockBaseAddress(buffer, kCVPixelBufferLock_ReadOnly);

        auto width = (int) CVPixelBufferGetWidth(buffer);
        auto height = (int) CVPixelBufferGetHeight(buffer);
        auto stride = CVPixelBufferGetBytesPerRow(buffer);
        const auto* base = (const std::uint8_t*) CVPixelBufferGetBaseAddress(buffer);

        out.width = width;
        out.height = height;
        out.data.resize(width * height * 4);

        auto rowBytes = (std::size_t) width * 4;

        for (auto y = 0; y < height; ++y)
            std::memcpy(out.data.data() + (std::size_t) y * rowBytes,
                        base + (std::size_t) y * stride,
                        rowBytes);

        CVPixelBufferUnlockBaseAddress(buffer, kCVPixelBufferLock_ReadOnly);
        CFRelease(buffer);

        out.sequence = copiedSequence;
        return true;
    }

    std::mutex mutex;
    CVPixelBufferRef current = nullptr;
    std::uint64_t sequence = 0;
};

NSDictionary* outputPixelBufferAttributes()
{
    // BGRA for the sprite pipeline, Metal-compatible (IOSurface-backed) so
    // Device::wrapPixelBuffer can wrap each frame zero-copy.
    return @{
        (id) kCVPixelBufferPixelFormatTypeKey : @(kCVPixelFormatType_32BGRA),
        (id) kCVPixelBufferMetalCompatibilityKey : @YES
    };
}
} // namespace

struct Player::Native
{
    explicit Native(Player& ownerToUse)
        : owner(ownerToUse)
    {
    }

    ~Native() { close(); }

    bool open(const FilePath& file)
    {
        ObjC::AutoReleasePool pool;
        close();

        auto* url =
            [NSURL fileURLWithPath:[NSString stringWithUTF8String:file.c_str()]];

        item = [[AVPlayerItem alloc] initWithURL:url];
        output = [[AVPlayerItemVideoOutput alloc]
            initWithPixelBufferAttributes:outputPixelBufferAttributes()];
        [item.get() addOutput:output.get()];

        player = [[AVPlayer alloc] initWithPlayerItem:item.get()];

        // Local files never stall for the network; without this a pre-ready
        // setRate would be deferred by the stall heuristics.
        player.get().automaticallyWaitsToMinimizeStalling = NO;
        player.get().actionAtItemEnd = AVPlayerActionAtItemEndPause;
        player.get().muted = muted ? YES : NO;
        player.get().volume = volume;

        installEndObserver();

        state = PlayerState::Loading;
        statusPoll.emplace([this] { pollStatus(); }, statusPollHz);
        startPacer();
        return true;
    }

    void close()
    {
        ObjC::AutoReleasePool pool;

        // Joined before anything it touches is released: the pacer messages
        // `output` every tick, so tearing that down underneath it would be a
        // use-after-free.
        stopPacer();

        // Anything already queued for the main thread describes the player
        // being torn down here, not the one a following open() builds.
        *alive = false;
        alive = std::make_shared<bool>(true);

        statusPoll.reset();
        removeEndObserver();

        if (player)
            [player.get() pause];

        if (item && output)
            [item.get() removeOutput:output.get()];

        player.release();
        output.release();
        item.release();

        latest.clear();
        state = PlayerState::Idle;
        videoWidth = 0;
        videoHeight = 0;
        videoDuration = 0.0;
    }

    // The end-of-item notification carries the loop restart; the observer
    // token is autoreleased by the API, so it is retained into the Ptr.
    void installEndObserver()
    {
        auto* token = [[NSNotificationCenter defaultCenter]
            addObserverForName:AVPlayerItemDidPlayToEndTimeNotification
                        object:item.get()
                         queue:[NSOperationQueue mainQueue]
                    usingBlock:^(NSNotification*) {
                        handleEnded();
                    }];

        endObserver.reset((NSObject*) token);
    }

    void removeEndObserver()
    {
        if (endObserver)
            [[NSNotificationCenter defaultCenter]
                removeObserver:endObserver.get()];

        endObserver.release();
    }

    void handleEnded()
    {
        if (looping && player)
        {
            [player.get() seekToTime:kCMTimeZero
                     toleranceBefore:kCMTimeZero
                      toleranceAfter:kCMTimeZero];
            [player.get() setRate:(float) rate.load()];
        }
        else
        {
            // actionAtItemEnd is Pause, so the clock has stopped — the pacer
            // must stop looking too, or a finished clip keeps it awake for good.
            playing = false;
        }

        owner.onEnded();
    }

    // Once loading resolves the poll has nothing left to watch, and a timer
    // left ticking at statusPollHz for the life of every player is pure waste.
    // It cannot be destroyed from inside its own callback — that frees the
    // block currently running — so the reset is deferred to the next main-thread
    // turn, with the token covering a player closed before it lands.
    void retireStatusPoll()
    {
        Threads::callAsync(
            [this, guard = alive]
            {
                if (*guard)
                    statusPoll.reset();
            });
    }

    // AVPlayerItem's status has no completion callback without KVO
    // machinery; a light main-thread poll keeps this file free of observer
    // classes. It retires itself once loading resolves; an open() from inside
    // onReady is safe, because its close() invalidates the token the pending
    // retirement rides on, so the fresh poll is not stopped underneath it.
    void pollStatus()
    {
        ObjC::AutoReleasePool pool;

        if (state != PlayerState::Loading || !item)
            return;

        auto status = item.get().status;

        if (status == AVPlayerItemStatusReadyToPlay)
        {
            auto size = item.get().presentationSize;
            videoWidth = (int) size.width;
            videoHeight = (int) size.height;

            auto total = item.get().duration;
            videoDuration = CMTIME_IS_NUMERIC(total) ? CMTimeGetSeconds(total)
                                                     : 0.0;

            nominalFps = readNominalFrameRate();

            state = PlayerState::Ready;
            retireStatusPoll();
            owner.onReady();
        }
        else if (status == AVPlayerItemStatusFailed)
        {
            state = PlayerState::Failed;
            retireStatusPoll();

            auto* message = item.get().error.localizedDescription;
            owner.onError(message != nil ? message.UTF8String
                                         : "failed to load video");
        }
    }

    // The video track's own frame rate, which the pacer's cadence follows.
    // Taken off AVPlayerItem's tracks rather than the asset's, so there is no
    // deprecated -tracksWithMediaType: and no synchronous asset load.
    double readNominalFrameRate() const
    {
        if (!item)
            return 0.0;

        for (AVPlayerItemTrack* track in item.get().tracks)
        {
            auto* assetTrack = track.assetTrack;

            if (assetTrack != nil &&
                [assetTrack.mediaType isEqualToString:AVMediaTypeVideo])
                return assetTrack.nominalFrameRate;
        }

        return 0.0;
    }

    // Pulls the frame due now (by the player's own clock) into `latest`.
    // Returns whether a new frame was actually published. Pacing thread only.
    bool refreshLatest()
    {
        ObjC::AutoReleasePool pool;

        if (!output)
            return false;

        auto itemTime = [output.get() itemTimeForHostTime:CACurrentMediaTime()];

        if (![output.get() hasNewPixelBufferForItemTime:itemTime])
            return false;

        auto buffer = [output.get() copyPixelBufferForItemTime:itemTime
                                           itemTimeForDisplay:nil];

        if (buffer == nullptr)
            return false;

        latest.set(buffer);
        CVBufferRelease(buffer);
        return true;
    }

    void setFrameArrived(Callback callbackToUse)
    {
        std::lock_guard<std::mutex> lock(arrivedMutex);
        frameArrived = std::move(callbackToUse);
    }

    // Copied out before invoking so the callback never runs under the lock —
    // it hops to the main thread and may re-enter this player.
    void notifyFrameArrived()
    {
        auto arrived = Callback {};

        {
            std::lock_guard<std::mutex> lock(arrivedMutex);
            arrived = frameArrived;
        }

        arrived();
    }

    // How often the pacer looks for a new frame. Twice the rate frames are
    // actually due at: the check is a timestamp comparison and costs nothing
    // when nothing is due, and oversampling means a frame is picked up within
    // half its interval instead of landing up to a whole one late.
    //
    // Scaled by the playback rate, because the pull cadence is the ceiling on
    // delivered frames — a fixed one would turn fast-forward into a slideshow,
    // dropping seven of every eight frames at 8x however fast the decoder ran.
    double pollInterval() const
    {
        auto tracked = nominalFps.load();
        auto fps = tracked > 0.0 ? tracked : 30.0;
        auto due = fps * std::max(0.25, std::abs(rate.load()));
        return 1.0 / std::clamp(due * 2.0, 30.0, 240.0);
    }

    void startPacer()
    {
        pacerQuit = false;
        pacer = std::thread([this] { runPacer(); });
    }

    void stopPacer()
    {
        if (!pacer.joinable())
            return;

        {
            std::lock_guard<std::mutex> lock(pacerMutex);
            pacerQuit = true;
        }

        pacerWake.notify_all();
        pacer.join();
    }

    // The analogue of the camera's capture queue: it owns the pull, publishes
    // each frame as the latest, and announces it. It sleeps outright while
    // paused, so a paused player costs nothing at all.
    void runPacer()
    {
        auto nextPoll = std::chrono::steady_clock::now();

        while (true)
        {
            {
                std::unique_lock<std::mutex> lock(pacerMutex);
                auto gate = [this]
                { return pacerQuit || playing || pullAttempts > 0; };

                if (!gate())
                {
                    pacerWake.wait(lock, gate);

                    // The schedule restarts from here: the one it was keeping
                    // describes a stretch this player spent paused.
                    nextPoll = std::chrono::steady_clock::now();
                }

                if (pacerQuit)
                    return;
            }

            auto published = refreshLatest();

            // A seek while paused still has to repaint the picture, so it asks
            // for a bounded run of attempts rather than leaving the view on the
            // pre-seek frame; the seek resolves asynchronously, so the frame is
            // not ready on the first try.
            if (!playing && pullAttempts > 0)
                pullAttempts = published ? 0 : pullAttempts - 1;

            if (published)
                notifyFrameArrived();

            // Stepped on an absolute schedule rather than slept for a fixed
            // interval: sleeping AFTER the work adds its cost to every period,
            // and the error accumulates — a 30fps clip drifted out to a
            // measured 37.7ms cadence instead of 33.3.
            auto interval = std::chrono::duration_cast<
                std::chrono::steady_clock::duration>(
                std::chrono::duration<double>(pollInterval()));

            nextPoll += interval;
            auto now = std::chrono::steady_clock::now();

            // A stall longer than a period restarts the cadence rather than
            // bursting through a backlog of missed polls to catch up.
            if (nextPoll <= now)
                nextPoll = now + interval;

            std::this_thread::sleep_until(nextPoll);
        }
    }

    void wakePacer()
    {
        // The lock is taken (and immediately dropped) so this serialises with
        // the wait's predicate check: the flags it gates on are set outside
        // the mutex, and a bare notify can land in the window between the
        // predicate reading them false and the wait blocking — lost, with
        // nothing left to wake the pacer again.
        {
            std::lock_guard<std::mutex> lock(pacerMutex);
        }

        pacerWake.notify_all();
    }

    void requestPull()
    {
        pullAttempts = pullAttemptsPerSeek;
        wakePacer();
    }

    Player& owner;

    // Mutable so const queries (currentTime, frameSequence) can message the
    // player and take the latest-frame lock through the const Pimpl.
    mutable ObjC::Ptr<AVPlayer> player;
    mutable ObjC::Ptr<AVPlayerItem> item;
    mutable ObjC::Ptr<AVPlayerItemVideoOutput> output;
    ObjC::Ptr<NSObject> endObserver;

    mutable LatestFrame latest;
    std::optional<Threads::Timer> statusPoll;
    static constexpr int statusPollHz = 30;

    // Fences work queued onto the main thread against a player torn down
    // before it runs. Replaced on close, so anything still queued backs off.
    std::shared_ptr<bool> alive = std::make_shared<bool>(true);

    // Unlike the owner's callbacks this is rewired while the player runs (a
    // VideoView attaching and detaching), so access is fenced.
    std::mutex arrivedMutex;
    Callback frameArrived = [] {};

    // The pacing thread and its wake-up state. `playing` is the pacer's gate,
    // so it is separate from the AVPlayer's own rate.
    std::thread pacer;
    std::mutex pacerMutex;
    std::condition_variable pacerWake;
    bool pacerQuit = false;
    std::atomic<bool> playing {false};

    // Bounded run of pulls after a seek while paused — see runPacer.
    static constexpr int pullAttemptsPerSeek = 30;
    std::atomic<int> pullAttempts {0};

    // The clip's own frame rate, which sets the pull cadence. Read once the
    // item is ready; 0 until then, where pollInterval falls back to 30.
    std::atomic<double> nominalFps {0.0};

    // Read by the pacing thread to scale its cadence, written from the main
    // thread by setRate.
    std::atomic<double> rate {1.0};

    PlayerState state = PlayerState::Idle;
    bool looping = false;
    bool muted = false;
    float volume = 1.0f;
    int videoWidth = 0;
    int videoHeight = 0;
    double videoDuration = 0.0;
};

Player::Player()
    : impl(*this)
{
}

Player::~Player() = default;

bool Player::open(const FilePath& file)
{
    return impl->open(file);
}

void Player::close()
{
    impl->close();
}

void Player::play()
{
    if (!impl->player)
        return;

    [impl->player.get() setRate:(float) impl->rate.load()];
    impl->playing = true;
    impl->wakePacer();
}

void Player::pause()
{
    if (!impl->player)
        return;

    [impl->player.get() pause];
    impl->playing = false;
}

bool Player::isPlaying() const
{
    return impl->player && impl->player.get().rate != 0.0f;
}

void Player::setLooping(bool shouldLoop)
{
    impl->looping = shouldLoop;
}

bool Player::isLooping() const
{
    return impl->looping;
}

void Player::setMuted(bool muted)
{
    impl->muted = muted;

    if (impl->player)
        impl->player.get().muted = muted ? YES : NO;
}

void Player::setVolume(float volume)
{
    impl->volume = volume;

    if (impl->player)
        impl->player.get().volume = volume;
}

void Player::setRate(double rate)
{
    impl->rate = rate;

    if (impl->player && isPlaying())
        [impl->player.get() setRate:(float) rate];
}

void Player::seek(double seconds)
{
    if (!impl->player)
        return;

    [impl->player.get() seekToTime:CMTimeMakeWithSeconds(seconds, 600)
                   toleranceBefore:kCMTimeZero
                    toleranceAfter:kCMTimeZero];

    // A seek while paused still has to repaint: nothing else would pull the
    // frame at the new position, leaving the view on the pre-seek picture.
    impl->requestPull();
}

PlayerState Player::state() const
{
    return impl->state;
}

int Player::width() const
{
    return impl->videoWidth;
}

int Player::height() const
{
    return impl->videoHeight;
}

double Player::duration() const
{
    return impl->videoDuration;
}

double Player::currentTime() const
{
    if (!impl->player)
        return 0.0;

    auto time = impl->player.get().currentTime;
    return CMTIME_IS_NUMERIC(time) ? CMTimeGetSeconds(time) : 0.0;
}

void Player::setFrameArrivedCallback(Callback callback)
{
    if (!callback)
        callback = [] {};

    impl->setFrameArrived(std::move(callback));
}

// The pull that used to sit here now belongs to the pacing thread, so these
// are pure reads of whatever it last published — the render path does no
// decoding.
void* Player::acquireLatestPixelBuffer()
{
    return impl->latest.acquire();
}

void Player::releasePixelBuffer(void* buffer)
{
    if (buffer != nullptr)
        CFRelease(buffer);
}

bool Player::copyLatestFrame(FramePixels& out)
{
    return impl->latest.copyInto(out);
}

std::uint64_t Player::frameSequence() const
{
    return impl->latest.currentSequence();
}
} // namespace eacp::Video

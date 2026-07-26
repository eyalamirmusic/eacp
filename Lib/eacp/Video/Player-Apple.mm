#import <AVFoundation/AVFoundation.h>
#import <QuartzCore/QuartzCore.h>

#include "Player.h"

#include <eacp/Core/ObjC/AutoReleasePool.h>
#include <eacp/Core/ObjC/ObjC.h>

#include <cstring>
#include <mutex>
#include <optional>

// macOS/iOS playback backend (AVFoundation). AVPlayer owns the clock, audio
// and A/V sync; an AVPlayerItemVideoOutput hands the display path BGRA
// CVPixelBuffers, pulled at render time (itemTimeForHostTime) and stashed as
// the latest frame exactly like the camera capture path. MRC throughout —
// every alloc/init is owned by an ObjC::Ptr.

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
    bool copyInto(PlayerFramePixels& out)
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
        return true;
    }

    void close()
    {
        ObjC::AutoReleasePool pool;

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
            [player.get() setRate:(float) rate];
        }

        owner.onEnded();
    }

    // AVPlayerItem's status has no completion callback without KVO
    // machinery; a light main-thread poll keeps this file free of observer
    // classes. It stops itself once loading resolves — the stop comes before
    // the owner callbacks, so an open() from inside onReady gets a fresh poll
    // that is not immediately stopped underneath it.
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

            state = PlayerState::Ready;
            statusPoll->stop();
            owner.onReady();
        }
        else if (status == AVPlayerItemStatusFailed)
        {
            state = PlayerState::Failed;
            statusPoll->stop();

            auto* message = item.get().error.localizedDescription;
            owner.onError(message != nil ? message.UTF8String
                                         : "failed to load video");
        }
    }

    // Pulls the frame due now (by the player's own clock) into `latest`.
    void refreshLatest()
    {
        ObjC::AutoReleasePool pool;

        if (!output)
            return;

        auto itemTime =
            [output.get() itemTimeForHostTime:CACurrentMediaTime()];

        if (![output.get() hasNewPixelBufferForItemTime:itemTime])
            return;

        auto buffer = [output.get() copyPixelBufferForItemTime:itemTime
                                            itemTimeForDisplay:nil];

        if (buffer != nullptr)
        {
            latest.set(buffer);
            CVBufferRelease(buffer);
        }
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

    PlayerState state = PlayerState::Idle;
    bool looping = false;
    bool muted = false;
    float volume = 1.0f;
    double rate = 1.0;
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
    if (impl->player)
        [impl->player.get() setRate:(float) impl->rate];
}

void Player::pause()
{
    if (impl->player)
        [impl->player.get() pause];
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
    if (impl->player)
        [impl->player.get() seekToTime:CMTimeMakeWithSeconds(seconds, 600)
                       toleranceBefore:kCMTimeZero
                        toleranceAfter:kCMTimeZero];
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

void* Player::acquireFramePixelBuffer()
{
    impl->refreshLatest();
    return impl->latest.acquire();
}

void Player::releasePixelBuffer(void* buffer)
{
    if (buffer != nullptr)
        CFRelease(buffer);
}

bool Player::copyLatestFrame(PlayerFramePixels& out)
{
    impl->refreshLatest();
    return impl->latest.copyInto(out);
}

std::uint64_t Player::frameSequence() const
{
    return impl->latest.currentSequence();
}
} // namespace eacp::Video

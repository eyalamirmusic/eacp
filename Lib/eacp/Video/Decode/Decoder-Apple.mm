#import <AVFoundation/AVFoundation.h>

#include "Decoder.h"

#include "FrameImage.h"

#include <eacp/Core/ObjC/ObjC.h>

#include <cmath>

// Apple decode backend (AVFoundation). AVAssetReader does demux, hardware
// decode, presentation reordering and the conversion to BGRA in one object, so
// this file is only the mapping between that and the portable Decoder
// interface: no timing, no queueing, no policy.
//
// Frames come out as IOSurface-backed, Metal-compatible CVPixelBuffers, which
// is exactly what GPU::Device::wrapPixelBuffer already turns into a texture
// without copying — the same path Cameras::CameraView uses for live capture.

namespace eacp::Video
{
namespace
{
// The reader's timeRange is the seek: AVAssetReader starts from the sync sample
// preceding the requested time and drops what precedes it, so a range starting
// mid-file yields the frame covering that time. That makes SeekMode::Accurate
// the natural behaviour and leaves Keyframe with nothing cheaper to do.
constexpr auto seekTimescale = 600;

int rotationFromTransform(CGAffineTransform transform)
{
    auto degrees = (int) std::lround(std::atan2(transform.b, transform.a) * 180.0
                                     / M_PI);
    return ((degrees % 360) + 360) % 360;
}

// The track load is asynchronous since macOS 15, but opening a decoder is a
// blocking operation by contract (Decoder::open returns success or failure), so
// the wait happens here. AVFoundation runs the completion on its own queue, so
// this is safe on any thread including the main one.
AVAssetTrack* firstVideoTrack(AVURLAsset* asset)
{
    __block AVAssetTrack* found = nil;
    auto* ready = dispatch_semaphore_create(0);

    [asset loadTracksWithMediaType:AVMediaTypeVideo
                 completionHandler:^(NSArray<AVAssetTrack*>* tracks, NSError*)
    {
        found = [tracks firstObject];
        dispatch_semaphore_signal(ready);
    }];

    dispatch_semaphore_wait(ready, DISPATCH_TIME_FOREVER);
    dispatch_release(ready);

    return found;
}
} // namespace

struct AppleDecoder final : Decoder
{
    bool open(const FilePath& path) override
    {
        auto* url = [NSURL fileURLWithPath:@(path.c_str())];
        auto* urlAsset = [AVURLAsset URLAssetWithURL:url options:nil];

        if (urlAsset == nil)
            return false;

        auto* videoTrack = firstVideoTrack(urlAsset);

        if (videoTrack == nil)
            return false;

        asset.reset(urlAsset);
        track.reset(videoTrack);

        auto size = track.get().naturalSize;
        videoInfo.width = (int) std::lround(size.width);
        videoInfo.height = (int) std::lround(size.height);
        videoInfo.duration = CMTimeGetSeconds(urlAsset.duration);
        videoInfo.frameRate = track.get().nominalFrameRate;
        videoInfo.rotationDegrees =
            rotationFromTransform(track.get().preferredTransform);

        if (!std::isfinite(videoInfo.duration) || videoInfo.duration < 0.0)
            videoInfo.duration = 0.0;

        return startReading(kCMTimeZero);
    }

    VideoInfo info() const override { return videoInfo; }

    bool nextFrame(VideoFrame& out) override
    {
        if (!output)
            return false;

        auto sample = [output.get() copyNextSampleBuffer];

        if (sample == nullptr)
            return false;

        auto pixelBuffer = CMSampleBufferGetImageBuffer(sample);

        if (pixelBuffer == nullptr)
        {
            CFRelease(sample);
            return false;
        }

        auto frameInfo = FrameInfo {};
        frameInfo.width = (int) CVPixelBufferGetWidth(pixelBuffer);
        frameInfo.height = (int) CVPixelBufferGetHeight(pixelBuffer);
        frameInfo.bytesPerRow = CVPixelBufferGetBytesPerRow(pixelBuffer);
        frameInfo.seconds = secondsOf(CMSampleBufferGetPresentationTimeStamp(sample));
        frameInfo.duration = secondsOf(CMSampleBufferGetOutputDuration(sample));

        // A container that gives no per-sample duration leaves the frame with no
        // presentation interval, which FrameStream needs to decide what belongs
        // on screen. The track's nominal rate is the decoder's own best answer,
        // so it is filled in here rather than guessed at higher up.
        if (frameInfo.duration <= 0.0 && videoInfo.frameRate > 0.0)
            frameInfo.duration = 1.0 / videoInfo.frameRate;

        // The frame owns the buffer from here: retained now, released by the
        // last VideoFrame sharing it. Retaining is also what stops the decoder's
        // pool recycling it underneath us while alwaysCopiesSampleData is off.
        CFRetain(pixelBuffer);
        out = VideoFrame::fromNativeBuffer(pixelBuffer,
                                           [](void* buffer) { CFRelease(buffer); },
                                           frameInfo);

        CFRelease(sample);
        return true;
    }

    // Note on timestamps: the reader clips its output to the range, so the
    // first frame after a seek reports the *seek time* rather than its own
    // position — seeking to 0.35 in a 10fps clip yields the frame covering
    // [0.3, 0.4) stamped 0.35. Its pixels are the right ones and playback is
    // unaffected, but a caller that needs a frame's true position (an editor
    // matching a cut to a source timestamp) cannot read it back off the first
    // frame after a seek. Fixing that means indexing sync samples ourselves,
    // which belongs with the keyframe-index work rather than here.
    void seek(double seconds, SeekMode) override
    {
        if (reader)
            [reader.get() cancelReading];

        startReading(CMTimeMakeWithSeconds(std::max(0.0, seconds), seekTimescale));
    }

private:
    static double secondsOf(CMTime time)
    {
        if (!CMTIME_IS_VALID(time) || CMTIME_IS_INDEFINITE(time))
            return 0.0;

        auto value = CMTimeGetSeconds(time);
        return std::isfinite(value) ? value : 0.0;
    }

    // Builds a reader over [from, end of file). Called for open() and for every
    // seek — AVAssetReader is single-pass, so repositioning means a new one.
    bool startReading(CMTime from)
    {
        reader.release();
        output.release();

        NSError* error = nil;
        auto* newReader = [AVAssetReader assetReaderWithAsset:asset.get()
                                                        error:&error];

        if (newReader == nil)
            return false;

        newReader.timeRange = CMTimeRangeMake(from, kCMTimePositiveInfinity);

        NSDictionary* settings = @{
            (id) kCVPixelBufferPixelFormatTypeKey : @(kCVPixelFormatType_32BGRA),
            (id) kCVPixelBufferMetalCompatibilityKey : @YES,
            (id) kCVPixelBufferIOSurfacePropertiesKey : @ {}
        };

        auto* newOutput =
            [AVAssetReaderTrackOutput assetReaderTrackOutputWithTrack:track.get()
                                                       outputSettings:settings];

        if (newOutput == nil || ![newReader canAddOutput:newOutput])
            return false;

        // Each frame's CVPixelBuffer is retained and outlives its sample buffer,
        // so there is nothing to gain from AVFoundation copying it as well.
        newOutput.alwaysCopiesSampleData = NO;

        [newReader addOutput:newOutput];

        if (![newReader startReading])
            return false;

        reader.reset(newReader);
        output.reset(newOutput);
        return true;
    }

    ObjC::Ptr<AVURLAsset> asset;
    ObjC::Ptr<AVAssetTrack> track;
    ObjC::Ptr<AVAssetReader> reader;
    ObjC::Ptr<AVAssetReaderTrackOutput> output;
    VideoInfo videoInfo;
};

OwningPointer<Decoder> makeDecoder()
{
    return makeOwned<AppleDecoder>();
}

Graphics::Image nativeBufferToImage(void* buffer)
{
    auto pixelBuffer = (CVPixelBufferRef) buffer;

    if (pixelBuffer == nullptr)
        return {};

    if (CVPixelBufferGetPixelFormatType(pixelBuffer) != kCVPixelFormatType_32BGRA)
        return {};

    CVPixelBufferLockBaseAddress(pixelBuffer, kCVPixelBufferLock_ReadOnly);

    auto width = (int) CVPixelBufferGetWidth(pixelBuffer);
    auto height = (int) CVPixelBufferGetHeight(pixelBuffer);
    auto stride = CVPixelBufferGetBytesPerRow(pixelBuffer);
    const auto* base = (const std::uint8_t*) CVPixelBufferGetBaseAddress(pixelBuffer);

    auto image = Graphics::Image {};

    if (base != nullptr && width > 0 && height > 0)
    {
        image = Graphics::Image {width, height};

        for (auto y = 0; y < height; ++y)
        {
            const auto* row = base + (std::size_t) y * stride;

            for (auto x = 0; x < width; ++x)
                image.set(x,
                          y,
                          Graphics::Color {(float) row[x * 4 + 2] / 255.0f,
                                           (float) row[x * 4 + 1] / 255.0f,
                                           (float) row[x * 4 + 0] / 255.0f,
                                           (float) row[x * 4 + 3] / 255.0f});
        }
    }

    CVPixelBufferUnlockBaseAddress(pixelBuffer, kCVPixelBufferLock_ReadOnly);
    return image;
}
} // namespace eacp::Video

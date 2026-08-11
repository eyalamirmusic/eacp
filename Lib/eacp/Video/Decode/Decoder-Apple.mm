#import <AVFoundation/AVFoundation.h>

#include "Decoder.h"

#include "FrameImage.h"

#include <eacp/Core/ObjC/ObjC.h>

#include <cmath>

// AVAssetReader hands back IOSurface-backed, Metal-compatible CVPixelBuffers,
// which wrapPixelBuffer turns into a texture without copying.

namespace eacp::Video
{
namespace
{
// The reader's timeRange is the seek: it starts from the preceding sync sample
// and drops what precedes the requested time, so Accurate comes for free and
// Keyframe has nothing cheaper to do.
constexpr auto seekTimescale = 600;

int rotationFromTransform(CGAffineTransform transform)
{
    auto degrees = (int) std::lround(std::atan2(transform.b, transform.a) * 180.0
                                     / M_PI);
    return ((degrees % 360) + 360) % 360;
}

// Blocks. AVFoundation runs the completion on its own queue, so this is safe on
// any thread including the main one.
API_AVAILABLE(macos(12.0), ios(15.0))
AVAssetTrack* awaitFirstVideoTrack(AVURLAsset* asset)
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

AVAssetTrack* firstVideoTrack(AVURLAsset* asset)
{
    if (@available(macOS 12.0, iOS 15.0, *))
        return awaitFirstVideoTrack(asset);

    return [[asset tracksWithMediaType:AVMediaTypeVideo] firstObject];
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

        // FrameStream needs a presentation interval even when the container
        // gives no per-sample duration.
        if (frameInfo.duration <= 0.0 && videoInfo.frameRate > 0.0)
            frameInfo.duration = 1.0 / videoInfo.frameRate;

        // The retain hands ownership to the frame and stops the decoder's pool
        // recycling the buffer while alwaysCopiesSampleData is off.
        CFRetain(pixelBuffer);
        out = VideoFrame::fromNativeBuffer(pixelBuffer,
                                           [](void* buffer) { CFRelease(buffer); },
                                           frameInfo);

        CFRelease(sample);
        return true;
    }

    // The reader clips output to the range, so the first frame after a seek has
    // the right pixels but reports the seek time as its own timestamp.
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

    // AVAssetReader is single-pass, so repositioning means a new one over
    // [from, end of file).
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

        // nextFrame retains each buffer, so it outlives its sample buffer.
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

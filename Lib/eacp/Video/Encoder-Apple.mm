#import <AppKit/AppKit.h>

#include "Encoder-Apple.h"

#include <eacp/Graphics/Graphics.h>

#include <cctype>
#include <cstdint>

namespace eacp::Video
{
namespace
{
NSString* fileTypeForPath(const FilePath& path)
{
    auto extension = path.extension();
    for (auto& c: extension)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));

    if (extension == ".mp4" || extension == "mp4")
        return AVFileTypeMPEG4;

    return AVFileTypeQuickTimeMovie;
}

// A standalone IOSurface-backed, Metal-compatible BGRA buffer (for the GpuDirect
// support probe, before the encoder's pool exists). Caller releases.
CVPixelBufferRef makeMetalPixelBuffer(int width, int height)
{
    NSDictionary* attributes = @{
        (id) kCVPixelBufferMetalCompatibilityKey : @YES,
        (id) kCVPixelBufferIOSurfacePropertiesKey : @ {}
    };

    CVPixelBufferRef buffer = nullptr;
    CVPixelBufferCreate(nullptr,
                        (size_t) width,
                        (size_t) height,
                        kCVPixelFormatType_32BGRA,
                        (CFDictionaryRef) attributes,
                        &buffer);
    return buffer;
}
} // namespace

bool AppleEncoder::begin(const FilePath& path, int w, int h, int bitrate, int)
{
    // fps is unused here: AVAssetWriter derives timing from each frame's
    // presentation timestamp (expectsMediaDataInRealTime + PTS).
    width = w;
    height = h;

    auto* url = [NSURL fileURLWithPath:@(path.c_str())];
    [[NSFileManager defaultManager] removeItemAtURL:url error:nil];

    NSError* error = nil;
    auto* writerObject = [[AVAssetWriter alloc] initWithURL:url
                                                   fileType:fileTypeForPath(path)
                                                      error:&error];
    if (writerObject == nil)
        return false;

    NSDictionary* compression = @{AVVideoAverageBitRateKey : @(bitrate)};
    NSDictionary* settings = @{
        AVVideoCodecKey : AVVideoCodecTypeH264,
        AVVideoWidthKey : @(width),
        AVVideoHeightKey : @(height),
        AVVideoCompressionPropertiesKey : compression
    };

    auto* in = [[AVAssetWriterInput alloc] initWithMediaType:AVMediaTypeVideo
                                              outputSettings:settings];
    in.expectsMediaDataInRealTime = YES;

    // IOSurface + Metal compatibility so the GpuDirect tier can render straight
    // into pool buffers; harmless for the CPU-filled snapshot tier.
    NSDictionary* pixelAttributes = @{
        (id) kCVPixelBufferPixelFormatTypeKey : @(kCVPixelFormatType_32BGRA),
        (id) kCVPixelBufferWidthKey : @(width),
        (id) kCVPixelBufferHeightKey : @(height),
        (id) kCVPixelBufferMetalCompatibilityKey : @YES,
        (id) kCVPixelBufferIOSurfacePropertiesKey : @ {}
    };

    auto* ad = [[AVAssetWriterInputPixelBufferAdaptor alloc]
        initWithAssetWriterInput:in
        sourcePixelBufferAttributes:pixelAttributes];

    auto ok = [writerObject canAddInput:in];
    if (ok)
    {
        [writerObject addInput:in];
        ok = [writerObject startWriting];
    }

    if (!ok)
    {
        [writerObject release];
        [in release];
        [ad release];
        return false;
    }

    writer = writerObject;
    input = in;
    adaptor = ad;
    return true;
}

CVPixelBufferPoolRef AppleEncoder::pool() const
{
    return adaptor.get().pixelBufferPool;
}

void AppleEncoder::append(CVPixelBufferRef buffer, CMTime pts)
{
    if (!sessionStarted)
    {
        [writer.get() startSessionAtSourceTime:pts];
        sessionStarted = true;
    }

    if ([input.get() isReadyForMoreMediaData])
        [adaptor.get() appendPixelBuffer:buffer withPresentationTime:pts];
}

void AppleEncoder::appendImage(const Graphics::Image& image, double ptsSeconds)
{
    auto* bufferPool = pool();
    if (bufferPool == nullptr)
        return;

    CVPixelBufferRef buffer = nullptr;
    if (CVPixelBufferPoolCreatePixelBuffer(nullptr, bufferPool, &buffer)
        != kCVReturnSuccess)
        return;

    CVPixelBufferLockBaseAddress(buffer, 0);

    auto* dst = static_cast<std::uint8_t*>(CVPixelBufferGetBaseAddress(buffer));
    auto dstStride = CVPixelBufferGetBytesPerRow(buffer);
    compositeOverBlackBGRA(image, dst, width, height, dstStride);

    CVPixelBufferUnlockBaseAddress(buffer, 0);

    append(buffer, CMTimeMakeWithSeconds(ptsSeconds, 600));
    CVPixelBufferRelease(buffer);
}

bool AppleEncoder::canCaptureNativeContent(Graphics::View& view,
                                           float scale,
                                           int probeWidth,
                                           int probeHeight)
{
    auto probeBuffer = makeMetalPixelBuffer(probeWidth, probeHeight);
    auto supported = probeBuffer != nullptr
                     && view.renderNativeContentToTarget(probeBuffer, scale);
    if (probeBuffer != nullptr)
        CVPixelBufferRelease(probeBuffer);

    return supported;
}

bool AppleEncoder::appendNativeContent(Graphics::View& view, float scale, double pts)
{
    auto* bufferPool = pool();
    if (bufferPool == nullptr)
        return false;

    CVPixelBufferRef buffer = nullptr;
    if (CVPixelBufferPoolCreatePixelBuffer(nullptr, bufferPool, &buffer)
        != kCVReturnSuccess)
        return false;

    auto captured = view.renderNativeContentToTarget(buffer, scale);
    if (captured)
        append(buffer, CMTimeMakeWithSeconds(pts, 600));

    CVPixelBufferRelease(buffer);
    return captured;
}

Threads::Async<void> AppleEncoder::finish()
{
    auto promise = Threads::AsyncPromise<void> {};
    auto result = promise.get();

    if (!writer)
    {
        promise.resolve();
        return result;
    }

    [input.get() markAsFinished];
    [writer.get() finishWritingWithCompletionHandler:^{
        Threads::callAsync([promise] { promise.resolve(); });
    }];

    return result;
}

OwningPointer<Encoder> makeEncoder()
{
    return makeOwned<AppleEncoder>();
}

} // namespace eacp::Video

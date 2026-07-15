#import <AVFoundation/AVFoundation.h>

#include "VideoRecorder.h"

#include <eacp/Core/ObjC/ObjC.h>
#include <eacp/Graphics/Graphics.h>

#include <cctype>
#include <cstdint>
#include <string>
#include <utility>

// AVFoundation recorder. A DisplayLink drives one snapshot per refresh
// (View::renderToImage), each converted from straight-alpha RGBA to BGRA (over
// black, so the video is opaque) into a pooled CVPixelBuffer and appended with a
// real-time PTS. finishWriting flushes the container asynchronously. MRC: every
// alloc/init is adopted by an ObjC::Ptr.

namespace eacp::Video
{
namespace
{
int roundDownToEven(int value)
{
    return value & ~1;
}

NSString* fileTypeForPath(const FilePath& path)
{
    auto extension = path.extension();
    for (auto& c: extension)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));

    if (extension == ".mp4" || extension == "mp4")
        return AVFileTypeMPEG4;

    return AVFileTypeQuickTimeMovie;
}
} // namespace

struct VideoRecorder::Native
{
    Graphics::View* view = nullptr;
    float scale = 0.0f;
    int width = 0;
    int height = 0;
    bool recording = false;

    // Seconds between captured frames (1 / fps), or 0 for the display rate.
    // `nextCapture` is the scheduled time of the next frame on an ideal grid, so
    // pacing does not drift against a faster refresh rate.
    double frameInterval = 0.0;
    double nextCapture = 0.0;

    ObjC::Ptr<AVAssetWriter> writer;
    ObjC::Ptr<AVAssetWriterInput> input;
    ObjC::Ptr<AVAssetWriterInputPixelBufferAdaptor> adaptor;

    // Declared last so it is destroyed first: the DisplayLink stops firing
    // before the writer it feeds is released.
    OwningPointer<Threads::DisplayLink> link;

    void captureFrame(Threads::FrameTime frameTime)
    {
        if (!recording)
            return;

        // Hold the target frame rate against a faster display: capture only when
        // the next scheduled slot is due, advancing the schedule by a fixed
        // interval so it doesn't drift. Resync if a slow frame put us behind.
        if (frameInterval > 0.0)
        {
            if (frameTime.time < nextCapture)
                return;

            nextCapture += frameInterval;
            if (nextCapture <= frameTime.time)
                nextCapture = frameTime.time + frameInterval;
        }

        if (![input.get() isReadyForMoreMediaData])
            return;

        auto image = view->renderToImage(scale);
        if (!image.isValid() || image.width() < width || image.height() < height)
            return;

        auto* pool = adaptor.get().pixelBufferPool;
        if (pool == nullptr)
            return;

        CVPixelBufferRef buffer = nullptr;
        if (CVPixelBufferPoolCreatePixelBuffer(nullptr, pool, &buffer)
            != kCVReturnSuccess)
            return;

        CVPixelBufferLockBaseAddress(buffer, 0);

        auto* dst = static_cast<std::uint8_t*>(CVPixelBufferGetBaseAddress(buffer));
        auto dstStride = CVPixelBufferGetBytesPerRow(buffer);
        auto srcStride = static_cast<std::size_t>(image.width()) * 4;
        const auto* src = image.pixels().data();

        for (auto y = 0; y < height; ++y)
        {
            const auto* s = src + static_cast<std::size_t>(y) * srcStride;
            auto* d = dst + static_cast<std::size_t>(y) * dstStride;

            for (auto x = 0; x < width; ++x)
            {
                auto r = s[x * 4 + 0];
                auto g = s[x * 4 + 1];
                auto b = s[x * 4 + 2];
                auto a = s[x * 4 + 3];

                // Straight RGBA over black -> premultiplied, opaque BGRA.
                auto overBlack = [&](std::uint8_t c) -> std::uint8_t
                { return static_cast<std::uint8_t>((c * a + 127) / 255); };

                d[x * 4 + 0] = overBlack(b);
                d[x * 4 + 1] = overBlack(g);
                d[x * 4 + 2] = overBlack(r);
                d[x * 4 + 3] = 255;
            }
        }

        CVPixelBufferUnlockBaseAddress(buffer, 0);

        auto pts = CMTimeMakeWithSeconds(frameTime.time, 600);
        [adaptor.get() appendPixelBuffer:buffer withPresentationTime:pts];
        CVPixelBufferRelease(buffer);
    }
};

VideoRecorder::VideoRecorder() = default;
VideoRecorder::~VideoRecorder() = default;

bool VideoRecorder::isRecording() const
{
    return impl->recording;
}

bool VideoRecorder::start(Graphics::View& view,
                          const FilePath& path,
                          const VideoOptions& options)
{
    if (impl->recording)
        return false;

    impl->view = &view;
    impl->scale = options.scale;

    // Probe one frame to size the video; renderToImage resolves the backing
    // scale itself when options.scale is 0.
    auto probe = view.renderToImage(options.scale);
    impl->width = roundDownToEven(probe.width());
    impl->height = roundDownToEven(probe.height());

    if (impl->width <= 0 || impl->height <= 0)
        return false;

    auto* url = [NSURL fileURLWithPath:@(path.c_str())];
    [[NSFileManager defaultManager] removeItemAtURL:url error:nil];

    NSError* error = nil;
    auto* writer = [[AVAssetWriter alloc] initWithURL:url
                                             fileType:fileTypeForPath(path)
                                                error:&error];
    if (writer == nil)
        return false;

    auto bitrate = options.bitrate > 0 ? options.bitrate
                                       : impl->width * impl->height * 8;

    NSDictionary* compression =
        @{AVVideoAverageBitRateKey : @(bitrate)};

    NSDictionary* settings = @{
        AVVideoCodecKey : AVVideoCodecTypeH264,
        AVVideoWidthKey : @(impl->width),
        AVVideoHeightKey : @(impl->height),
        AVVideoCompressionPropertiesKey : compression
    };

    auto* input = [[AVAssetWriterInput alloc] initWithMediaType:AVMediaTypeVideo
                                                 outputSettings:settings];
    input.expectsMediaDataInRealTime = YES;

    NSDictionary* pixelAttributes = @{
        (id) kCVPixelBufferPixelFormatTypeKey : @(kCVPixelFormatType_32BGRA),
        (id) kCVPixelBufferWidthKey : @(impl->width),
        (id) kCVPixelBufferHeightKey : @(impl->height)
    };

    auto* adaptor = [[AVAssetWriterInputPixelBufferAdaptor alloc]
        initWithAssetWriterInput:input
        sourcePixelBufferAttributes:pixelAttributes];

    if (![writer canAddInput:input])
    {
        [writer release];
        [input release];
        [adaptor release];
        return false;
    }

    [writer addInput:input];

    if (![writer startWriting])
    {
        [writer release];
        [input release];
        [adaptor release];
        return false;
    }

    [writer startSessionAtSourceTime:kCMTimeZero];

    impl->writer = writer;
    impl->input = input;
    impl->adaptor = adaptor;
    impl->frameInterval = options.fps > 0 ? 1.0 / options.fps : 0.0;
    impl->nextCapture = 0.0;
    impl->recording = true;

    auto* native = impl.get();
    impl->link = makeOwned<Threads::DisplayLink>(
        [native](Threads::FrameTime time) { native->captureFrame(time); });

    return true;
}

Threads::Async<void> VideoRecorder::stop()
{
    auto promise = Threads::AsyncPromise<void> {};
    auto result = promise.get();

    if (!impl->recording)
    {
        promise.resolve();
        return result;
    }

    impl->recording = false;
    impl->link = nullptr; // stop capturing

    [impl->input.get() markAsFinished];

    [impl->writer.get() finishWritingWithCompletionHandler:^{
        // Fires on an AVFoundation queue; hop back to the main thread to resolve.
        Threads::callAsync([promise] { promise.resolve(); });
    }];

    return result;
}

} // namespace eacp::Video

#import <AVFoundation/AVFoundation.h>
#import <AppKit/AppKit.h>
#import <ScreenCaptureKit/ScreenCaptureKit.h>

#include "VideoRecorder.h"

#include <eacp/Core/ObjC/ObjC.h>
#include <eacp/Graphics/Graphics.h>

#include <cctype>
#include <cmath>
#include <cstdint>
#include <functional>

// Two capture tiers behind one API:
//   Snapshot -- a DisplayLink drives View::renderToImage each refresh; the RGBA
//     snapshot is composited over black into a pooled CVPixelBuffer (BGRA) and
//     appended with a real-time PTS. Portable, off-screen, any content.
//   Screen   -- ScreenCaptureKit taps the WindowServer's live composite of the
//     view's host window (2D + GPU + WebView), delivering IOSurface-backed
//     CVPixelBuffers straight to the encoder. Real-time, GPU-side; needs the
//     window on-screen and Screen Recording permission.
// Both feed a shared AVAssetWriter (H.264). MRC: alloc/init adopted by ObjC::Ptr.

namespace
{
int roundDownToEven(int value)
{
    return value & ~1;
}
} // namespace

// ScreenCaptureKit sample sink. Forwards each complete frame's CVPixelBuffer +
// PTS to a C++ callback (set right after construction).
API_AVAILABLE(macos(12.3))
@interface EacpScreenSink : NSObject <SCStreamOutput, SCStreamDelegate>
@end

@implementation EacpScreenSink
{
@public
    std::function<void(CVPixelBufferRef, CMTime)> onFrame;
}

- (void)stream:(SCStream*)stream
    didOutputSampleBuffer:(CMSampleBufferRef)sampleBuffer
                   ofType:(SCStreamOutputType)type
{
    if (type != SCStreamOutputTypeScreen || !CMSampleBufferIsValid(sampleBuffer))
        return;

    // Only "complete" frames carry a fresh surface; skip idle/blank deliveries.
    auto* attachments =
        (NSArray*) CMSampleBufferGetSampleAttachmentsArray(sampleBuffer, NO);
    if (NSDictionary* info = attachments.firstObject)
    {
        auto status = (SCFrameStatus) [info[SCStreamFrameInfoStatus] integerValue];
        if (status != SCFrameStatusComplete)
            return;
    }

    auto buffer = (CVPixelBufferRef) CMSampleBufferGetImageBuffer(sampleBuffer);
    if (buffer != nullptr && onFrame)
        onFrame(buffer, CMSampleBufferGetPresentationTimeStamp(sampleBuffer));
}

- (void)stream:(SCStream*)stream didStopWithError:(NSError*)error
{
}
@end

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

// Shared H.264 encoder: an AVAssetWriter fed BGRA CVPixelBuffers with PTS. The
// writer session starts at the first appended frame's timestamp, so both a
// zero-based snapshot clock and ScreenCaptureKit's host clock work unchanged.
struct Encoder
{
    ObjC::Ptr<AVAssetWriter> writer;
    ObjC::Ptr<AVAssetWriterInput> input;
    ObjC::Ptr<AVAssetWriterInputPixelBufferAdaptor> adaptor;
    bool sessionStarted = false;

    bool valid() const { return writer && input && adaptor; }

    bool begin(const FilePath& path, int width, int height, int bitrate)
    {
        auto* url = [NSURL fileURLWithPath:@(path.c_str())];
        [[NSFileManager defaultManager] removeItemAtURL:url error:nil];

        NSError* error = nil;
        auto* w = [[AVAssetWriter alloc] initWithURL:url
                                            fileType:fileTypeForPath(path)
                                               error:&error];
        if (w == nil)
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

        NSDictionary* pixelAttributes = @{
            (id) kCVPixelBufferPixelFormatTypeKey : @(kCVPixelFormatType_32BGRA),
            (id) kCVPixelBufferWidthKey : @(width),
            (id) kCVPixelBufferHeightKey : @(height)
        };

        auto* ad = [[AVAssetWriterInputPixelBufferAdaptor alloc]
            initWithAssetWriterInput:in
            sourcePixelBufferAttributes:pixelAttributes];

        auto ok = [w canAddInput:in];
        if (ok)
        {
            [w addInput:in];
            ok = [w startWriting];
        }

        if (!ok)
        {
            [w release];
            [in release];
            [ad release];
            return false;
        }

        writer = w;
        input = in;
        adaptor = ad;
        return true;
    }

    CVPixelBufferPoolRef pool() const { return adaptor.get().pixelBufferPool; }

    void append(CVPixelBufferRef buffer, CMTime pts)
    {
        if (!sessionStarted)
        {
            [writer.get() startSessionAtSourceTime:pts];
            sessionStarted = true;
        }

        if ([input.get() isReadyForMoreMediaData])
            [adaptor.get() appendPixelBuffer:buffer withPresentationTime:pts];
    }

    Threads::Async<void> finish()
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
};
} // namespace

struct VideoRecorder::Native
{
    CaptureMode mode = CaptureMode::Snapshot;
    Encoder encoder;
    bool recording = false;

    // --- Snapshot tier ---
    Graphics::View* view = nullptr;
    float scale = 0.0f;
    int width = 0;
    int height = 0;
    double frameInterval = 0.0;
    double nextCapture = 0.0;
    OwningPointer<Threads::DisplayLink> link;

    // --- Screen tier ---
    ObjC::Ptr<SCStream> stream API_AVAILABLE(macos(12.3));
    ObjC::Ptr<EacpScreenSink> sink API_AVAILABLE(macos(12.3));
    dispatch_queue_t sampleQueue = nullptr;

    void captureFrame(Threads::FrameTime frameTime)
    {
        if (!recording)
            return;

        // Hold the target frame rate against a faster display: capture only when
        // the next scheduled slot is due, advancing on an ideal grid so it does
        // not drift. Resync if a slow frame put us behind.
        if (frameInterval > 0.0)
        {
            if (frameTime.time < nextCapture)
                return;

            nextCapture += frameInterval;
            if (nextCapture <= frameTime.time)
                nextCapture = frameTime.time + frameInterval;
        }

        auto image = view->renderToImage(scale);
        if (!image.isValid() || image.width() < width || image.height() < height)
            return;

        auto* pool = encoder.pool();
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

        encoder.append(buffer, CMTimeMakeWithSeconds(frameTime.time, 600));
        CVPixelBufferRelease(buffer);
    }

    bool startSnapshot(Graphics::View& viewToUse,
                       const FilePath& path,
                       const VideoOptions& options)
    {
        view = &viewToUse;
        scale = options.scale;

        // Probe one frame to size the video; renderToImage resolves the backing
        // scale itself when options.scale is 0.
        auto probe = viewToUse.renderToImage(options.scale);
        width = roundDownToEven(probe.width());
        height = roundDownToEven(probe.height());

        if (width <= 0 || height <= 0)
            return false;

        auto bitrate = options.bitrate > 0 ? options.bitrate : width * height * 8;
        if (!encoder.begin(path, width, height, bitrate))
            return false;

        frameInterval = options.fps > 0 ? 1.0 / options.fps : 0.0;
        nextCapture = 0.0;
        recording = true;

        auto* native = this;
        link = makeOwned<Threads::DisplayLink>(
            [native](Threads::FrameTime time) { native->captureFrame(time); });

        return true;
    }

    bool startScreen(Graphics::View& viewToUse,
                     const FilePath& path,
                     const VideoOptions& options)
    {
        if (@available(macOS 12.3, *))
        {
            auto* nsView = (NSView*) viewToUse.getHandle();
            auto* nsWindow = nsView.window;
            if (nsWindow == nil)
                return false; // not hosted in a window; nothing to screen-capture

            auto windowID = (CGWindowID) nsWindow.windowNumber;
            auto backingScale =
                options.scale > 0 ? options.scale : (float) nsWindow.backingScaleFactor;

            auto fps = options.fps > 0 ? options.fps : 60;
            auto explicitBitrate = options.bitrate;
            auto outputPath = path;

            auto* sinkObject = [[EacpScreenSink alloc] init];
            auto* native = this;
            sinkObject->onFrame = [native](CVPixelBufferRef buffer, CMTime pts)
            {
                if (native->recording)
                    native->encoder.append(buffer, pts);
            };
            sink = sinkObject;

            sampleQueue =
                dispatch_queue_create("eacp.video.screen", DISPATCH_QUEUE_SERIAL);
            recording = true;

            // Enumerating shareable content is async and also drives the Screen
            // Recording permission check.
            [SCShareableContent
                getShareableContentWithCompletionHandler:^(SCShareableContent* content,
                                                           NSError* error) {
                    if (error != nil || content == nil)
                    {
                        LOG("VideoRecorder: screen content unavailable (permission?)");
                        return;
                    }

                    if (!native->recording)
                        return; // stopped before setup finished

                    SCWindow* target = nil;
                    for (SCWindow* candidate in content.windows)
                        if (candidate.windowID == windowID)
                        {
                            target = candidate;
                            break;
                        }

                    if (target == nil)
                    {
                        LOG("VideoRecorder: host window not shareable (off-screen?)");
                        return;
                    }

                    native->width = roundDownToEven(
                        (int) std::lround(target.frame.size.width * backingScale));
                    native->height = roundDownToEven(
                        (int) std::lround(target.frame.size.height * backingScale));

                    auto bitrate = explicitBitrate > 0
                                       ? explicitBitrate
                                       : native->width * native->height * 8;

                    if (native->width <= 0 || native->height <= 0
                        || !native->encoder.begin(
                            outputPath, native->width, native->height, bitrate))
                    {
                        LOG("VideoRecorder: encoder setup failed");
                        return;
                    }

                    auto* filter = [[SCContentFilter alloc]
                        initWithDesktopIndependentWindow:target];

                    auto* config = [[SCStreamConfiguration alloc] init];
                    config.width = (size_t) native->width;
                    config.height = (size_t) native->height;
                    config.pixelFormat = kCVPixelFormatType_32BGRA;
                    config.minimumFrameInterval = CMTimeMake(1, fps);
                    config.showsCursor = NO;
                    config.queueDepth = 6;

                    auto* newStream =
                        [[SCStream alloc] initWithFilter:filter
                                           configuration:config
                                                delegate:native->sink.get()];

                    NSError* addError = nil;
                    [newStream addStreamOutput:native->sink.get()
                                          type:SCStreamOutputTypeScreen
                            sampleHandlerQueue:native->sampleQueue
                                         error:&addError];

                    [filter release];
                    [config release];

                    if (addError != nil)
                    {
                        LOG("VideoRecorder: addStreamOutput failed");
                        [newStream release];
                        return;
                    }

                    native->stream = newStream;

                    [newStream startCaptureWithCompletionHandler:^(NSError* startError) {
                        if (startError != nil)
                            LOG("VideoRecorder: screen capture start failed");
                        else
                            LOG("VideoRecorder: screen capture started");
                    }];
                }];

            return true;
        }

        LOG("VideoRecorder: ScreenCaptureKit requires macOS 12.3+");
        return false;
    }

    Threads::Async<void> stopScreen()
    {
        auto promise = Threads::AsyncPromise<void> {};
        auto result = promise.get();
        auto* native = this;

        if (@available(macOS 12.3, *))
        {
            if (stream)
            {
                [stream.get() stopCaptureWithCompletionHandler:^(NSError*) {
                    // No more samples arrive after this; finalize on the main thread.
                    Threads::callAsync([native, promise] {
                        native->encoder.finish().then([promise] { promise.resolve(); });
                    });
                }];

                return result;
            }
        }

        // Stream never started (permission/off-screen): finalize whatever exists.
        return encoder.finish();
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

    impl->mode = options.mode;

    if (options.mode == CaptureMode::Screen)
        return impl->startScreen(view, path, options);

    return impl->startSnapshot(view, path, options);
}

Threads::Async<void> VideoRecorder::stop()
{
    if (!impl->recording)
    {
        auto promise = Threads::AsyncPromise<void> {};
        promise.resolve();
        return promise.get();
    }

    impl->recording = false;

    if (impl->mode == CaptureMode::Screen)
        return impl->stopScreen();

    impl->link = nullptr; // stop the snapshot display link
    return impl->encoder.finish();
}

} // namespace eacp::Video

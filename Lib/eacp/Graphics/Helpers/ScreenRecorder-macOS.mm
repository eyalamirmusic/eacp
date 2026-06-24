#include "ScreenRecorder.h"

#include "../View/View.h"
#include "../Window/Window.h"

#include <cmath>
#include <filesystem>
#include <string>

#import <AVFoundation/AVFoundation.h>
#import <Cocoa/Cocoa.h>
#import <CoreMedia/CoreMedia.h>
#import <ImageIO/ImageIO.h>
#import <ScreenCaptureKit/ScreenCaptureKit.h>

@class EacpRecorderOutput;

namespace eacp::Graphics
{

// Captures the window through ScreenCaptureKit (the supported window
// capture API on modern macOS) and hands each delivered frame's pixel
// buffer straight to AVAssetWriter — no intermediate image copy and no
// reliance on the obsoleted CGWindowList APIs.
struct ScreenRecorder::Native
{
    SCStream* stream = nil;
    EacpRecorderOutput* output = nil;
    dispatch_queue_t queue = nil;

    AVAssetWriter* writer = nil;
    AVAssetWriterInput* input = nil;
    AVAssetWriterInputPixelBufferAdaptor* adaptor = nil;

    bool recording = false;
    bool sessionStarted = false;
    int fps = 30;
    std::string path;

    // Called on the capture queue for every delivered frame.
    void handleSampleBuffer(CMSampleBufferRef sampleBuffer)
    {
        if (!recording || !CMSampleBufferDataIsReady(sampleBuffer))
            return;

        auto* attachments = (__bridge NSArray*) CMSampleBufferGetSampleAttachmentsArray(
            sampleBuffer, false);
        if (attachments.count == 0)
            return;

        // Only "complete" frames carry fresh pixels; idle / blank frames
        // (sent when nothing changed) would just bloat the file.
        auto status = static_cast<SCFrameStatus>(
            [attachments[0][SCStreamFrameInfoStatus] integerValue]);
        if (status != SCFrameStatusComplete)
            return;

        auto pixelBuffer = CMSampleBufferGetImageBuffer(sampleBuffer);
        if (!pixelBuffer)
            return;

        auto pts = CMSampleBufferGetPresentationTimeStamp(sampleBuffer);

        // Start the movie's timeline at the first real frame, so the
        // video plays back in wall-clock time.
        if (!sessionStarted)
        {
            [writer startSessionAtSourceTime:pts];
            sessionStarted = true;
        }

        if (input.isReadyForMoreMediaData)
            [adaptor appendPixelBuffer:pixelBuffer withPresentationTime:pts];
    }
};

} // namespace eacp::Graphics

@interface EacpRecorderOutput : NSObject <SCStreamOutput, SCStreamDelegate>
// Set by the recorder; ScreenRecorder::Native is private, so frames are
// forwarded through a block rather than a typed back-pointer.
@property (nonatomic, copy) void (^onScreenSample)(CMSampleBufferRef);
@end

@implementation EacpRecorderOutput
- (void)stream:(SCStream*)stream
    didOutputSampleBuffer:(CMSampleBufferRef)sampleBuffer
                   ofType:(SCStreamOutputType)type
{
    if (type == SCStreamOutputTypeScreen && self.onScreenSample)
        self.onScreenSample(sampleBuffer);
}

- (void)stream:(SCStream*)stream didStopWithError:(NSError*)error
{
}
@end

namespace eacp::Graphics
{

namespace
{
void setError(std::string* error, const std::string& message)
{
    if (error)
        *error = message;
}

[[nodiscard]] bool fail(std::string* error, const std::string& message)
{
    setError(error, message);
    return false;
}

// Blocks the caller until an async ScreenCaptureKit handler signals, or
// the timeout elapses. SCKit completion handlers run on background
// queues, so this never deadlocks the main thread.
bool waitFor(dispatch_semaphore_t semaphore, double seconds)
{
    auto deadline =
        dispatch_time(DISPATCH_TIME_NOW, (int64_t) (seconds * NSEC_PER_SEC));
    return dispatch_semaphore_wait(semaphore, deadline) == 0;
}

struct ResolvedWindow
{
    SCWindow* window = nil;
    int width = 0;
    int height = 0;
};

// Blocking: find the on-screen window ScreenCaptureKit knows about and
// compute its pixel dimensions. An empty content list / missing window
// means it isn't visible or the process lacks Screen Recording
// permission. Shared by the recorder and the one-shot capture.
bool resolveWindow(NSWindow* nsWindow, ResolvedWindow& out, std::string* error)
    API_AVAILABLE(macos(12.3))
{
    auto windowID = static_cast<CGWindowID>(nsWindow.windowNumber);

    __block SCWindow* target = nil;
    __block NSError* contentError = nil;
    auto ready = dispatch_semaphore_create(0);
    [SCShareableContent
        getShareableContentWithCompletionHandler:^(SCShareableContent* content,
                                                   NSError* err) {
            if (err)
                contentError = err;
            else
                for (SCWindow* candidate in content.windows)
                    if (candidate.windowID == windowID)
                    {
                        target = candidate;
                        break;
                    }
            dispatch_semaphore_signal(ready);
        }];
    waitFor(ready, 5.0);

    if (contentError)
        return fail(error,
                    std::string {"ScreenRecorder: "}
                        + contentError.localizedDescription.UTF8String);
    if (!target)
        return fail(error,
                    "ScreenRecorder: could not find the window — it must be "
                    "visible and the process needs Screen Recording permission");

    auto scale = nsWindow.backingScaleFactor;
    auto width = static_cast<int>(std::llround(target.frame.size.width * scale));
    auto height = static_cast<int>(std::llround(target.frame.size.height * scale));
    width -= width & 1; // H.264 / encoders want even dimensions
    height -= height & 1;
    if (width <= 0 || height <= 0)
        return fail(error, "ScreenRecorder: window has no size");

    out = {target, width, height};
    return true;
}

Image cgImageToImage(CGImageRef cgImage, std::string* error)
{
    auto* data = [NSMutableData data];
    auto destination = CGImageDestinationCreateWithData(
        (__bridge CFMutableDataRef) data, (__bridge CFStringRef) @"public.png", 1, nullptr);
    if (!destination)
    {
        setError(error, "ScreenRecorder: could not create a PNG encoder");
        return {};
    }

    CGImageDestinationAddImage(destination, cgImage, nullptr);
    auto encoded = CGImageDestinationFinalize(destination);
    CFRelease(destination);

    if (!encoded)
    {
        setError(error, "ScreenRecorder: PNG encode failed");
        return {};
    }

    return Image::decode(static_cast<const std::uint8_t*>(data.bytes),
                         static_cast<int>(data.length),
                         error);
}

Image captureWindowImage(void* nativeWindow, std::string* error)
{
    auto* nsWindow = (__bridge NSWindow*) nativeWindow;
    if (!nsWindow)
    {
        setError(error, "ScreenRecorder: no native window to capture");
        return {};
    }

    if (@available(macOS 14, *))
    {
        auto resolved = ResolvedWindow {};
        if (!resolveWindow(nsWindow, resolved, error))
            return {};

        auto* config = [[SCStreamConfiguration alloc] init];
        config.width = static_cast<size_t>(resolved.width);
        config.height = static_cast<size_t>(resolved.height);
        config.pixelFormat = kCVPixelFormatType_32BGRA;
        config.showsCursor = NO;

        auto* filter =
            [[SCContentFilter alloc] initWithDesktopIndependentWindow:resolved.window];

        __block CGImageRef shot = nullptr;
        __block NSError* shotError = nil;
        auto ready = dispatch_semaphore_create(0);
        [SCScreenshotManager
            captureImageWithFilter:filter
                     configuration:config
                 completionHandler:^(CGImageRef image, NSError* err) {
                     if (image)
                         shot = (CGImageRef) CFRetain(image);
                     shotError = err;
                     dispatch_semaphore_signal(ready);
                 }];
        waitFor(ready, 5.0);

        if (shotError || !shot)
        {
            setError(error,
                     shotError ? std::string {"ScreenRecorder: "}
                                     + shotError.localizedDescription.UTF8String
                               : std::string {"ScreenRecorder: capture "
                                              "returned no image"});
            if (shot)
                CFRelease(shot);
            return {};
        }

        auto image = cgImageToImage(shot, error);
        CFRelease(shot);
        return image;
    }

    setError(error, "ScreenRecorder: window image capture requires macOS 14+");
    return {};
}
} // namespace

ScreenRecorder::ScreenRecorder() = default;
ScreenRecorder::~ScreenRecorder()
{
    stop();
}

bool ScreenRecorder::isRecording() const
{
    return impl->recording;
}

bool ScreenRecorder::start(Window& window,
                           const std::string& path,
                           Options options,
                           std::string* error)
{
    return startWindow(window.getHandle(), path, options, error);
}

bool ScreenRecorder::start(View& view,
                           const std::string& path,
                           Options options,
                           std::string* error)
{
    auto* nsView = (__bridge NSView*) view.getHandle();
    return startWindow(nsView ? (__bridge void*) nsView.window : nullptr,
                       path,
                       options,
                       error);
}

bool ScreenRecorder::startWindow(void* nativeWindow,
                                 const std::string& path,
                                 Options options,
                                 std::string* error)
{
    if (impl->recording)
        return fail(error, "ScreenRecorder: already recording");

    auto* nsWindow = (__bridge NSWindow*) nativeWindow;
    if (!nsWindow)
        return fail(error, "ScreenRecorder: no native window to record");

    if (@available(macOS 12.3, *))
    {
        impl->fps = options.frameRateHz > 0 ? options.frameRateHz : 30;
        impl->path = path;

        auto resolved = ResolvedWindow {};
        if (!resolveWindow(nsWindow, resolved, error))
            return false;

        auto width = resolved.width;
        auto height = resolved.height;

        auto fsPath = std::filesystem::path {path};
        if (fsPath.has_parent_path())
            std::filesystem::create_directories(fsPath.parent_path());
        std::filesystem::remove(fsPath);

        auto* url = [NSURL fileURLWithPath:@(path.c_str())];
        NSError* writerError = nil;
        impl->writer = [AVAssetWriter assetWriterWithURL:url
                                                fileType:AVFileTypeMPEG4
                                                   error:&writerError];
        if (!impl->writer)
            return fail(error,
                        std::string {"ScreenRecorder: "}
                            + writerError.localizedDescription.UTF8String);

        impl->input = [AVAssetWriterInput
            assetWriterInputWithMediaType:AVMediaTypeVideo
                           outputSettings:@{
                               AVVideoCodecKey : AVVideoCodecTypeH264,
                               AVVideoWidthKey : @(width),
                               AVVideoHeightKey : @(height),
                           }];
        impl->input.expectsMediaDataInRealTime = YES;
        impl->adaptor = [AVAssetWriterInputPixelBufferAdaptor
            assetWriterInputPixelBufferAdaptorWithAssetWriterInput:impl->input
                                       sourcePixelBufferAttributes:@{
                                           (id) kCVPixelBufferPixelFormatTypeKey :
                                               @(kCVPixelFormatType_32BGRA),
                                       }];
        [impl->writer addInput:impl->input];
        if (![impl->writer startWriting])
        {
            impl->writer = nil;
            impl->input = nil;
            impl->adaptor = nil;
            return fail(error, "ScreenRecorder: failed to start writing");
        }

        auto* config = [[SCStreamConfiguration alloc] init];
        config.width = static_cast<size_t>(width);
        config.height = static_cast<size_t>(height);
        config.pixelFormat = kCVPixelFormatType_32BGRA;
        config.minimumFrameInterval = CMTimeMake(1, impl->fps);
        config.queueDepth = 6;
        config.showsCursor = NO;

        auto* filter =
            [[SCContentFilter alloc] initWithDesktopIndependentWindow:resolved.window];

        impl->output = [[EacpRecorderOutput alloc] init];
        auto* native = impl.get();
        impl->output.onScreenSample = ^(CMSampleBufferRef sampleBuffer) {
            native->handleSampleBuffer(sampleBuffer);
        };
        impl->queue =
            dispatch_queue_create("ai.eacp.screenrecorder", DISPATCH_QUEUE_SERIAL);
        impl->stream = [[SCStream alloc] initWithFilter:filter
                                          configuration:config
                                               delegate:impl->output];

        NSError* addError = nil;
        [impl->stream addStreamOutput:impl->output
                                 type:SCStreamOutputTypeScreen
                   sampleHandlerQueue:impl->queue
                                error:&addError];
        if (addError)
        {
            impl->stream = nil;
            impl->output = nil;
            impl->writer = nil;
            impl->input = nil;
            impl->adaptor = nil;
            return fail(error,
                        std::string {"ScreenRecorder: "}
                            + addError.localizedDescription.UTF8String);
        }

        impl->recording = true;

        __block NSError* startError = nil;
        auto started = dispatch_semaphore_create(0);
        [impl->stream startCaptureWithCompletionHandler:^(NSError* err) {
            startError = err;
            dispatch_semaphore_signal(started);
        }];
        waitFor(started, 5.0);

        if (startError)
        {
            impl->recording = false;
            impl->stream = nil;
            impl->output = nil;
            impl->writer = nil;
            impl->input = nil;
            impl->adaptor = nil;
            return fail(error,
                        std::string {"ScreenRecorder: "}
                            + startError.localizedDescription.UTF8String);
        }

        return true;
    }

    return fail(error, "ScreenRecorder: screen recording requires macOS 12.3+");
}

std::string ScreenRecorder::stop()
{
    if (!impl->recording)
        return {};

    impl->recording = false;

    if (@available(macOS 12.3, *))
    {
        auto stopped = dispatch_semaphore_create(0);
        [impl->stream stopCaptureWithCompletionHandler:^(NSError*) {
            dispatch_semaphore_signal(stopped);
        }];
        waitFor(stopped, 5.0);
    }

    [impl->input markAsFinished];

    auto finished = dispatch_semaphore_create(0);
    [impl->writer finishWritingWithCompletionHandler:^{
        dispatch_semaphore_signal(finished);
    }];
    waitFor(finished, 10.0);

    auto path = impl->path;
    impl->stream = nil;
    impl->output = nil;
    impl->queue = nil;
    impl->writer = nil;
    impl->input = nil;
    impl->adaptor = nil;
    impl->sessionStarted = false;

    return path;
}

Image captureWindowImage(Window& window, std::string* error)
{
    return captureWindowImage(window.getHandle(), error);
}

Image captureViewImage(View& view, std::string* error)
{
    auto* nsView = (__bridge NSView*) view.getHandle();
    return captureWindowImage(nsView ? (__bridge void*) nsView.window : nullptr,
                              error);
}

} // namespace eacp::Graphics

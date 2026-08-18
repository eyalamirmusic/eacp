#import <AppKit/AppKit.h>
#import <ScreenCaptureKit/ScreenCaptureKit.h>

#include "Encoder-Apple.h"
#include "ScreenCapture.h"

#include <eacp/Graphics/Graphics.h>

#include <cmath>
#include <functional>

// The Screen tier on macOS: ScreenCaptureKit taps the WindowServer's live
// composite of the view's host window (2D + GPU + WebView) and delivers
// IOSurface-backed CVPixelBuffers straight to the shared AppleEncoder. Real-time,
// GPU-side; needs the window on-screen and Screen Recording permission.

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
int roundDownToEven(int value)
{
    return value & ~1;
}

struct AppleScreenCapture final : ScreenCapture
{
    bool start(Graphics::View& view,
               const FilePath& path,
               const RecordingOptions& options,
               Encoder& encoderToUse) override
    {
        if (@available(macOS 12.3, *))
        {
            // Asked before anything is set up, because everything past this
            // point reports failure asynchronously or not at all: without the
            // grant, SCShareableContent simply hands back nothing.
            if (!hasScreenCapturePermission())
            {
                LOG("VideoRecorder: no Screen Recording permission");
                return false;
            }

            encoder = static_cast<AppleEncoder*>(&encoderToUse);

            auto* nsView = (NSView*) view.getHandle();
            auto* nsWindow = nsView.window;
            if (nsWindow == nil)
                return false; // not hosted in a window; nothing to screen-capture

            auto windowID = (CGWindowID) nsWindow.windowNumber;
            auto backingScale =
                options.scale > 0 ? options.scale : (float) nsWindow.backingScaleFactor;

            auto fps = options.fps > 0 ? options.fps : 60;
            auto explicitBitrate = options.bitrate;
            auto audioSpec = options.audio;
            auto outputPath = path;

            // The recorder's timeline starts here, so the host-clock stamps
            // ScreenCaptureKit hands back are rebased onto it -- which is what
            // puts them on the same zero as the audio the app pushes.
            timelineStart = CMClockGetTime(CMClockGetHostTimeClock());

            auto* sinkObject = [[EacpScreenSink alloc] init];
            auto* self = this;
            sinkObject->onFrame = [self](CVPixelBufferRef buffer, CMTime pts)
            {
                if (self->active)
                    self->encoder->append(buffer,
                                          CMTimeSubtract(pts, self->timelineStart));
            };
            sink = sinkObject;

            sampleQueue =
                dispatch_queue_create("eacp.video.screen", DISPATCH_QUEUE_SERIAL);
            active = true;

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

                    if (!self->active)
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

                    self->width = roundDownToEven(
                        (int) std::lround(target.frame.size.width * backingScale));
                    self->height = roundDownToEven(
                        (int) std::lround(target.frame.size.height * backingScale));

                    auto encoderSpec = EncoderSpec {};
                    encoderSpec.video.width = self->width;
                    encoderSpec.video.height = self->height;
                    encoderSpec.video.fps = fps;
                    encoderSpec.video.bitrate =
                        explicitBitrate > 0 ? explicitBitrate
                                            : self->width * self->height * 8;
                    encoderSpec.audio = audioSpec;

                    if (self->width <= 0 || self->height <= 0
                        || !self->encoder->begin(outputPath, encoderSpec))
                    {
                        LOG("VideoRecorder: encoder setup failed");
                        return;
                    }

                    auto* filter = [[SCContentFilter alloc]
                        initWithDesktopIndependentWindow:target];

                    auto* config = [[SCStreamConfiguration alloc] init];
                    config.width = (size_t) self->width;
                    config.height = (size_t) self->height;
                    config.pixelFormat = kCVPixelFormatType_32BGRA;
                    config.minimumFrameInterval = CMTimeMake(1, fps);
                    config.showsCursor = NO;
                    config.queueDepth = 6;

                    auto* streamSink = (EacpScreenSink*) self->sink.get();

                    auto* newStream =
                        [[SCStream alloc] initWithFilter:filter
                                           configuration:config
                                                delegate:streamSink];

                    NSError* addError = nil;
                    [newStream addStreamOutput:streamSink
                                          type:SCStreamOutputTypeScreen
                            sampleHandlerQueue:self->sampleQueue
                                         error:&addError];

                    [filter release];
                    [config release];

                    if (addError != nil)
                    {
                        LOG("VideoRecorder: addStreamOutput failed");
                        [newStream release];
                        return;
                    }

                    self->stream = newStream;

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

    Threads::Async<void> stop() override
    {
        active = false;

        auto promise = Threads::AsyncPromise<void> {};
        auto result = promise.get();
        auto* self = this;

        if (@available(macOS 12.3, *))
        {
            if (stream)
            {
                [(SCStream*) stream.get() stopCaptureWithCompletionHandler:^(NSError*) {
                    // No more samples arrive after this; finalize on the main thread.
                    Threads::callAsync([self, promise] {
                        self->encoder->finish().then([promise] { promise.resolve(); });
                    });
                }];

                return result;
            }
        }

        // Stream never started (permission/off-screen): finalize whatever exists.
        return encoder->finish();
    }

    // An availability attribute on a member does not satisfy clang — it wants
    // the whole enclosing struct annotated, which this one cannot be: it is
    // constructed on every macOS version and answers false from start() on the
    // ones without ScreenCaptureKit. Storing the two 12.3-only objects as their
    // common base keeps the declarations version-free, and every use of them
    // already sits inside an @available guard that can cast back.
    ObjC::Ptr<NSObject> stream;
    ObjC::Ptr<NSObject> sink;
    dispatch_queue_t sampleQueue = nullptr;
    CMTime timelineStart = kCMTimeZero;
    AppleEncoder* encoder = nullptr;
    bool active = false;
    int width = 0;
    int height = 0;
};
} // namespace

bool hasScreenCapturePermission()
{
    return CGPreflightScreenCaptureAccess();
}

void requestScreenCapturePermission(std::function<void(bool)> onResult)
{
    // The request goes through the TCC daemon, so it is kept off the main
    // thread and the answer marshalled back the way the camera's is.
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0),
                   ^{
                       auto granted = CGRequestScreenCaptureAccess() == TRUE;

                       Threads::callAsync(
                           [onResult, granted]
                           {
                               if (onResult)
                                   onResult(granted);
                           });
                   });
}

OwningPointer<ScreenCapture> makeScreenCapture()
{
    return makeOwned<AppleScreenCapture>();
}

} // namespace eacp::Video

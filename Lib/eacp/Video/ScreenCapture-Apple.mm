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
               const VideoOptions& options,
               Encoder& encoderToUse) override
    {
        if (@available(macOS 12.3, *))
        {
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
            auto outputPath = path;

            auto* sinkObject = [[EacpScreenSink alloc] init];
            auto* self = this;
            sinkObject->onFrame = [self](CVPixelBufferRef buffer, CMTime pts)
            {
                if (self->active)
                    self->encoder->append(buffer, pts);
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

                    auto bitrate = explicitBitrate > 0
                                       ? explicitBitrate
                                       : self->width * self->height * 8;

                    if (self->width <= 0 || self->height <= 0
                        || !self->encoder->begin(
                            outputPath, self->width, self->height, bitrate, fps))
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

                    auto* newStream =
                        [[SCStream alloc] initWithFilter:filter
                                           configuration:config
                                                delegate:self->sink.get()];

                    NSError* addError = nil;
                    [newStream addStreamOutput:self->sink.get()
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
                [stream.get() stopCaptureWithCompletionHandler:^(NSError*) {
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

    ObjC::Ptr<SCStream> stream API_AVAILABLE(macos(12.3));
    ObjC::Ptr<EacpScreenSink> sink API_AVAILABLE(macos(12.3));
    dispatch_queue_t sampleQueue = nullptr;
    AppleEncoder* encoder = nullptr;
    bool active = false;
    int width = 0;
    int height = 0;
};
} // namespace

OwningPointer<ScreenCapture> makeScreenCapture()
{
    return makeOwned<AppleScreenCapture>();
}

} // namespace eacp::Video

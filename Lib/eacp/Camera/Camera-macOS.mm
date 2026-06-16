#import <AVFoundation/AVFoundation.h>

#include "Camera.h"

#include <eacp/Core/ObjC/AutoReleasePool.h>
#include <eacp/Core/ObjC/ObjC.h>
#include <eacp/Core/Threads/EventLoop.h>

#include <algorithm>
#include <atomic>
#include <cmath>

// macOS capture backend (AVFoundation). One AVCaptureVideoDataOutput delivers
// 32-bit BGRA frames on a dedicated serial queue; the delegate wraps each
// CVPixelBuffer in a CameraFrame view (valid only for the callback) and hands it
// to the user callback. alwaysDiscardsLateVideoFrames provides the drop-stale
// backpressure. MRC throughout — every alloc/init is owned by an ObjC::Ptr.

namespace eacp::Cameras
{
// The capture queue calls back into this; Camera::Native owns it and points the
// delegate at it. The callback is set before start(), so the capture thread only
// reads it.
struct CaptureContext
{
    FrameCallback callback;
};
} // namespace eacp::Cameras

@interface EacpCameraDelegate
    : NSObject <AVCaptureVideoDataOutputSampleBufferDelegate>
{
@public
    eacp::Cameras::CaptureContext* context;
}
@end

@implementation EacpCameraDelegate
- (void)captureOutput:(AVCaptureOutput*)output
    didOutputSampleBuffer:(CMSampleBufferRef)sampleBuffer
           fromConnection:(AVCaptureConnection*)connection
{
    using namespace eacp::Cameras;

    if (context == nullptr || !context->callback)
        return;

    auto imageBuffer = CMSampleBufferGetImageBuffer(sampleBuffer);

    if (imageBuffer == nullptr)
        return;

    auto pixelBuffer = (CVPixelBufferRef) imageBuffer;

    CVPixelBufferLockBaseAddress(pixelBuffer, kCVPixelBufferLock_ReadOnly);

    auto width = (int) CVPixelBufferGetWidth(pixelBuffer);
    auto height = (int) CVPixelBufferGetHeight(pixelBuffer);
    auto bytesPerRow = CVPixelBufferGetBytesPerRow(pixelBuffer);
    auto base = (const std::uint8_t*) CVPixelBufferGetBaseAddress(pixelBuffer);

    auto presentation = CMSampleBufferGetPresentationTimeStamp(sampleBuffer);
    auto timestamp =
        CMTIME_IS_VALID(presentation) ? CMTimeGetSeconds(presentation) : 0.0;

    auto frame = CameraFrame(width,
                             height,
                             PixelFormat::BGRA8,
                             bytesPerRow,
                             timestamp,
                             base,
                             pixelBuffer);

    context->callback(frame);

    CVPixelBufferUnlockBaseAddress(pixelBuffer, kCVPixelBufferLock_ReadOnly);
}
@end

namespace eacp::Cameras
{
namespace
{
AVCaptureSessionPreset presetFor(int width, int height)
{
    if (width >= 1920 || height >= 1080)
        return AVCaptureSessionPreset1920x1080;

    if (width >= 1280 || height >= 720)
        return AVCaptureSessionPreset1280x720;

    if (width >= 640 || height >= 480)
        return AVCaptureSessionPreset640x480;

    return AVCaptureSessionPresetHigh;
}

AVCaptureDevice* resolveDevice(const std::optional<std::string>& deviceId)
{
    if (deviceId.has_value())
    {
        auto* uid = [NSString stringWithUTF8String:deviceId->c_str()];
        auto* device = [AVCaptureDevice deviceWithUniqueID:uid];

        if (device != nil)
            return device;
    }

    return [AVCaptureDevice defaultDeviceWithMediaType:AVMediaTypeVideo];
}

NSArray<AVCaptureDeviceType>* discoveryDeviceTypes()
{
    auto* types = [NSMutableArray<AVCaptureDeviceType> array];
    [types addObject:AVCaptureDeviceTypeBuiltInWideAngleCamera];

    if (@available(macOS 14.0, *))
        [types addObject:AVCaptureDeviceTypeExternal];

    return types;
}

void applyFrameRate(AVCaptureDevice* device, double frameRate)
{
    if (frameRate <= 0.0)
        return;

    NSError* error = nil;

    if (![device lockForConfiguration:&error])
        return;

    auto duration = CMTimeMake(1, (int32_t) std::llround(frameRate));

    for (AVFrameRateRange* range in device.activeFormat
             .videoSupportedFrameRateRanges)
    {
        if (frameRate >= range.minFrameRate && frameRate <= range.maxFrameRate)
        {
            device.activeVideoMinFrameDuration = duration;
            device.activeVideoMaxFrameDuration = duration;
            break;
        }
    }

    [device unlockForConfiguration];
}
} // namespace

struct Camera::Native
{
    Native() = default;
    ~Native() { stop(); }

    void setFrameCallback(FrameCallback callback)
    {
        context.callback = std::move(callback);
    }

    bool start(const CameraConfig& config)
    {
        ObjC::AutoReleasePool pool;

        if (running.load())
            return true;

        auto* device = resolveDevice(config.deviceId);

        if (device == nil)
            return false;

        NSError* error = nil;
        auto* input = [AVCaptureDeviceInput deviceInputWithDevice:device
                                                            error:&error];

        if (input == nil)
            return false;

        session = [[AVCaptureSession alloc] init];
        [session.get() beginConfiguration];
        [session.get() setSessionPreset:presetFor(config.width, config.height)];

        if (![session.get() canAddInput:input])
        {
            [session.get() commitConfiguration];
            teardown();
            return false;
        }

        [session.get() addInput:input];

        output = [[AVCaptureVideoDataOutput alloc] init];
        output.get().videoSettings = @{
            (id) kCVPixelBufferPixelFormatTypeKey : @(kCVPixelFormatType_32BGRA)
        };
        output.get().alwaysDiscardsLateVideoFrames =
            config.discardLateFrames ? YES : NO;

        queue = dispatch_queue_create("com.eacp.camera.capture", DISPATCH_QUEUE_SERIAL);

        delegate = [[EacpCameraDelegate alloc] init];
        delegate.get()->context = &context;
        [output.get() setSampleBufferDelegate:delegate.get() queue:queue];

        if (![session.get() canAddOutput:output.get()])
        {
            [session.get() commitConfiguration];
            teardown();
            return false;
        }

        [session.get() addOutput:output.get()];
        [session.get() commitConfiguration];

        applyFrameRate(device, config.frameRate);

        // startRunning blocks while the device warms up; acceptable for the
        // capture probe. The display path (CameraView) moves it off the main
        // thread in a later phase.
        [session.get() startRunning];
        running.store([session.get() isRunning]);

        if (!running.load())
            teardown();

        return running.load();
    }

    void stop()
    {
        ObjC::AutoReleasePool pool;

        if (session && [session.get() isRunning])
            [session.get() stopRunning];

        teardown();
        running.store(false);
    }

    void teardown()
    {
        if (output)
            [output.get() setSampleBufferDelegate:nil queue:nullptr];

        // Drain any delegate call still in flight before the context it points
        // at goes away.
        if (queue != nullptr)
            dispatch_sync(queue, ^ {
            });

        delegate.release();
        output.release();
        session.release();

        if (queue != nullptr)
        {
            dispatch_release(queue);
            queue = nullptr;
        }
    }

    CaptureContext context;
    ObjC::Ptr<AVCaptureSession> session;
    ObjC::Ptr<AVCaptureVideoDataOutput> output;
    ObjC::Ptr<EacpCameraDelegate> delegate;
    dispatch_queue_t queue = nullptr;
    std::atomic<bool> running {false};
};

Camera::Camera()
    : impl()
{
}

Camera::~Camera() = default;

Vector<CameraDevice> Camera::devices()
{
    ObjC::AutoReleasePool pool;

    auto result = Vector<CameraDevice> {};

    auto* discovery = [AVCaptureDeviceDiscoverySession
        discoverySessionWithDeviceTypes:discoveryDeviceTypes()
                              mediaType:AVMediaTypeVideo
                               position:AVCaptureDevicePositionUnspecified];

    for (AVCaptureDevice* device in discovery.devices)
    {
        auto info = CameraDevice {};
        info.id = device.uniqueID.UTF8String;
        info.name = device.localizedName.UTF8String;
        info.isFrontFacing = device.position == AVCaptureDevicePositionFront;
        result.push_back(std::move(info));
    }

    return result;
}

Vector<CameraFormat> Camera::supportedFormats(const CameraDevice& device)
{
    ObjC::AutoReleasePool pool;

    auto result = Vector<CameraFormat> {};

    auto* uid = [NSString stringWithUTF8String:device.id.c_str()];
    auto* avDevice = [AVCaptureDevice deviceWithUniqueID:uid];

    if (avDevice == nil)
        return result;

    for (AVCaptureDeviceFormat* format in avDevice.formats)
    {
        auto dimensions = CMVideoFormatDescriptionGetDimensions(
            (CMVideoFormatDescriptionRef) format.formatDescription);

        auto info = CameraFormat {};
        info.width = dimensions.width;
        info.height = dimensions.height;
        info.pixelFormat = PixelFormat::BGRA8;

        auto maxRate = 0.0;

        for (AVFrameRateRange* range in format.videoSupportedFrameRateRanges)
            maxRate = std::max(maxRate, range.maxFrameRate);

        info.maxFrameRate = maxRate;
        result.push_back(info);
    }

    return result;
}

PermissionStatus Camera::permissionStatus()
{
    switch ([AVCaptureDevice authorizationStatusForMediaType:AVMediaTypeVideo])
    {
        case AVAuthorizationStatusAuthorized:
            return PermissionStatus::Granted;
        case AVAuthorizationStatusDenied:
            return PermissionStatus::Denied;
        case AVAuthorizationStatusRestricted:
            return PermissionStatus::Restricted;
        case AVAuthorizationStatusNotDetermined:
            return PermissionStatus::NotDetermined;
    }

    return PermissionStatus::NotDetermined;
}

void Camera::requestPermission(std::function<void(bool)> onResult)
{
    [AVCaptureDevice
        requestAccessForMediaType:AVMediaTypeVideo
                completionHandler:^(BOOL granted) {
                    auto result = granted == YES;
                    Threads::callAsync(
                        [onResult, result]
                        {
                            if (onResult)
                                onResult(result);
                        });
                }];
}

void Camera::setFrameCallback(FrameCallback callback)
{
    impl->setFrameCallback(std::move(callback));
}

bool Camera::start(const CameraConfig& config)
{
    return impl->start(config);
}

void Camera::stop()
{
    impl->stop();
}

bool Camera::isRunning() const
{
    return impl->running.load();
}

void* Camera::nativeSession() const
{
    return (__bridge void*) impl->session.get();
}
} // namespace eacp::Cameras

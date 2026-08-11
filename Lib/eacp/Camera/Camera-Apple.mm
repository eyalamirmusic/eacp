#import <AVFoundation/AVFoundation.h>
#include <algorithm>

#include "Camera.h"
#include "CameraDevices-Apple.h"

#include <eacp/Core/ObjC/AutoReleasePool.h>
#include <eacp/Core/ObjC/ObjC.h>
#include <eacp/Core/ObjC/RuntimeClass.h>

#include <atomic>
#include <cmath>
#include <cstring>
#include <mutex>

// AVFoundation capture backend. MRC throughout — every alloc/init is owned by
// an ObjC::Ptr.

namespace eacp::Cameras
{
// Set on the capture thread, acquired on the render thread.
struct LatestFrame
{
    ~LatestFrame()
    {
        if (current != nullptr)
            CFRelease(current);
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

    // Tightly packed BGRA, copied outside the lock so the capture thread never
    // blocks on the memcpy.
    bool copyInto(FramePixels& out)
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
        out.format = PixelFormat::BGRA8;
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

// Owned by Camera::Native, pointed at by the delegate. `callback` is set before
// start(), so the capture thread only ever reads it.
struct CaptureContext
{
    void setFrameArrived(Callback callbackToUse)
    {
        std::lock_guard<std::mutex> lock(arrivedMutex);
        frameArrived = std::move(callbackToUse);
    }

    void notifyFrameArrived()
    {
        auto arrived = Callback {};

        {
            std::lock_guard<std::mutex> lock(arrivedMutex);
            arrived = frameArrived;
        }

        arrived();
    }

    FrameCallback callback;
    LatestFrame latest;

    // Rewired while capture runs as CameraView attaches and detaches.
    std::mutex arrivedMutex;
    Callback frameArrived = [] {};
};

namespace
{
void cameraDelegateCaptureOutput(id self,
                                 SEL,
                                 AVCaptureOutput*,
                                 CMSampleBufferRef sampleBuffer,
                                 AVCaptureConnection*)
{
    auto* context = (CaptureContext*) ObjC::getIvar<void*>(self, "context");

    if (context == nullptr)
        return;

    auto imageBuffer = CMSampleBufferGetImageBuffer(sampleBuffer);

    if (imageBuffer == nullptr)
        return;

    auto pixelBuffer = (CVPixelBufferRef) imageBuffer;

    // The display path wraps the IOSurface on the GPU, so publish the buffer
    // before taking the CPU-side lock the raw callback needs.
    context->latest.set(pixelBuffer);
    context->notifyFrameArrived();

    if (!context->callback)
        return;

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

Class getCameraDelegateClass()
{
    static auto instance = []
    {
        auto builder = new ObjC::RuntimeClass<NSObject>("EacpCameraDelegate");

        builder->addIvar<void*>("context");
        builder->addProtocol(
            @protocol(AVCaptureVideoDataOutputSampleBufferDelegate));

        builder->addMethod(
            @selector(captureOutput:didOutputSampleBuffer:fromConnection:),
            cameraDelegateCaptureOutput);

        builder->registerClass();
        return builder;
    }();

    return instance->get();
}

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

bool formatSupportsRate(AVCaptureDeviceFormat* format, double frameRate)
{
    for (AVFrameRateRange* range in format.videoSupportedFrameRateRanges)
        if (frameRate >= range.minFrameRate && frameRate <= range.maxFrameRate)
            return true;

    return false;
}

CGSize formatDimensions(AVCaptureDeviceFormat* format)
{
    auto dims = CMVideoFormatDescriptionGetDimensions(
        (CMVideoFormatDescriptionRef) format.formatDescription);

    return CGSizeMake(dims.width, dims.height);
}

// Smallest format covering the requested size, else the largest available; nil
// when no format supports the rate.
AVCaptureDeviceFormat*
    bestFormatForRate(AVCaptureDevice* device, int width, int height, double frameRate)
{
    AVCaptureDeviceFormat* smallestCovering = nil;
    AVCaptureDeviceFormat* largest = nil;

    for (AVCaptureDeviceFormat* format in device.formats)
    {
        if (!formatSupportsRate(format, frameRate))
            continue;

        auto size = formatDimensions(format);
        auto area = size.width * size.height;

        if (largest == nil || area > formatDimensions(largest).width
                                          * formatDimensions(largest).height)
            largest = format;

        if (size.width >= width && size.height >= height)
        {
            if (smallestCovering == nil
                || area < formatDimensions(smallestCovering).width
                              * formatDimensions(smallestCovering).height)
                smallestCovering = format;
        }
    }

    return smallestCovering != nil ? smallestCovering : largest;
}

// macOS 720p/1080p presets usually cap at 30fps, so a higher requested rate
// needs an explicit format. Falls back to the active format's default rate.
void configureFormatAndFrameRate(AVCaptureDevice* device,
                                 int width,
                                 int height,
                                 double frameRate)
{
    if (frameRate <= 0.0)
        return;

    NSError* error = nil;

    if (![device lockForConfiguration:&error])
        return;

    if (!formatSupportsRate(device.activeFormat, frameRate))
    {
        if (auto* format = bestFormatForRate(device, width, height, frameRate))
            device.activeFormat = format;
    }

    if (formatSupportsRate(device.activeFormat, frameRate))
    {
        auto duration = CMTimeMake(1, (int32_t) std::llround(frameRate));
        device.activeVideoMinFrameDuration = duration;
        device.activeVideoMaxFrameDuration = duration;
    }

    [device unlockForConfiguration];

    auto size = formatDimensions(device.activeFormat);
    auto maxRate = 0.0;

    for (AVFrameRateRange* range in device.activeFormat
             .videoSupportedFrameRateRanges)
        maxRate = std::max(maxRate, range.maxFrameRate);

    LOG("[camera] active format ",
        (int) size.width,
        "x",
        (int) size.height,
        " formatMaxFps=",
        maxRate,
        " requestedFps=",
        frameRate);
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

        captureQueue =
            dispatch_queue_create("com.eacp.camera.capture", DISPATCH_QUEUE_SERIAL);

        delegate = [[getCameraDelegateClass() alloc] init];
        ObjC::getIvar<void*>(delegate.get(), "context") = &context;
        [output.get()
            setSampleBufferDelegate:
                (id<AVCaptureVideoDataOutputSampleBufferDelegate>) delegate.get()
                              queue:captureQueue];

        if (![session.get() canAddOutput:output.get()])
        {
            [session.get() commitConfiguration];
            teardown();
            return false;
        }

        [session.get() addOutput:output.get()];
        [session.get() commitConfiguration];

        configureFormatAndFrameRate(
            device, config.width, config.height, config.frameRate);

        // startRunning blocks while the device warms up, so keep it off the
        // caller's thread; stop() serialises with it on the same queue.
        sessionQueue =
            dispatch_queue_create("com.eacp.camera.session", DISPATCH_QUEUE_SERIAL);

        auto* sessionPtr = session.get();
        auto* runningPtr = &running;

        dispatch_async(sessionQueue, ^ {
            [sessionPtr startRunning];
            runningPtr->store([sessionPtr isRunning]);
        });

        return true;
    }

    void stop()
    {
        ObjC::AutoReleasePool pool;

        // Serialise with a pending startRunning before tearing anything down.
        if (sessionQueue != nullptr && session)
        {
            auto* sessionPtr = session.get();

            dispatch_sync(sessionQueue, ^ {
                if ([sessionPtr isRunning])
                    [sessionPtr stopRunning];
            });
        }

        teardown();
        running.store(false);
    }

    void teardown()
    {
        if (output)
            [output.get() setSampleBufferDelegate:nil queue:nullptr];

        // Drain any in-flight delegate call before its context goes away.
        if (captureQueue != nullptr)
            dispatch_sync(captureQueue, ^ {
            });

        delegate.release();
        output.release();
        session.release();

        if (captureQueue != nullptr)
        {
            dispatch_release(captureQueue);
            captureQueue = nullptr;
        }

        if (sessionQueue != nullptr)
        {
            dispatch_release(sessionQueue);
            sessionQueue = nullptr;
        }
    }

    CaptureContext context;
    ObjC::Ptr<AVCaptureSession> session;
    ObjC::Ptr<AVCaptureVideoDataOutput> output;
    ObjC::Ptr<NSObject> delegate;
    dispatch_queue_t captureQueue = nullptr;
    dispatch_queue_t sessionQueue = nullptr;
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
        discoverySessionWithDeviceTypes:platformDiscoveryDeviceTypes()
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

void Camera::setFrameArrivedCallback(Callback callback)
{
    if (!callback)
        callback = [] {};

    impl->context.setFrameArrived(std::move(callback));
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

void* Camera::acquireLatestPixelBuffer()
{
    return impl->context.latest.acquire();
}

void Camera::releasePixelBuffer(void* buffer)
{
    if (buffer != nullptr)
        CFRelease(buffer);
}

bool Camera::copyLatestFrame(FramePixels& out)
{
    return impl->context.latest.copyInto(out);
}
} // namespace eacp::Cameras

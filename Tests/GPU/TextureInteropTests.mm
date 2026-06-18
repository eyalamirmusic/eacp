#import <CoreVideo/CoreVideo.h>

#include <eacp/Core/ObjC/AutoReleasePool.h>
#include <eacp/GPU/GPU.h>

#include <NanoTest/NanoTest.h>

using namespace nano;
using namespace eacp;
using namespace eacp::GPU;

// Device::wrapPixelBuffer maps an IOSurface-backed CVPixelBuffer straight into a
// sampleable Metal texture with no copy — the camera/video display path. The
// buffer is created Metal-compatible and IOSurface-backed, the same shape an
// AVFoundation capture frame carries. Self-skips on a host with no Metal device
// (some headless CI VMs), like the other GPU tests.
auto tWrapsPixelBuffer = test("GPU/wrapsPixelBuffer") = []
{
    ObjC::AutoReleasePool pool;

    auto& device = Device::shared();

    if (!device.isValid())
        return;

    NSDictionary* attributes = @{
        (id) kCVPixelBufferMetalCompatibilityKey : @YES,
        (id) kCVPixelBufferIOSurfacePropertiesKey : @ {}
    };

    CVPixelBufferRef pixelBuffer = nullptr;
    auto status = CVPixelBufferCreate(kCFAllocatorDefault,
                                      4,
                                      4,
                                      kCVPixelFormatType_32BGRA,
                                      (__bridge CFDictionaryRef) attributes,
                                      &pixelBuffer);

    check(status == kCVReturnSuccess);
    check(pixelBuffer != nullptr);

    if (pixelBuffer == nullptr)
        return;

    auto texture = device.wrapPixelBuffer(pixelBuffer);
    check(texture.isValid());
    check(texture.width() == 4);
    check(texture.height() == 4);

    CVPixelBufferRelease(pixelBuffer);
};

#import <CoreVideo/CoreVideo.h>

#include "Common.h"
#include <eacp/Core/ObjC/AutoReleasePool.h>

using namespace nano;
using namespace eacp;
using namespace eacp::GPU;

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

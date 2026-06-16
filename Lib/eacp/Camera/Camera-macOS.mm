#import <AVFoundation/AVFoundation.h>

#include "CameraDevices-Apple.h"

// macOS-specific camera pieces. The capture pipeline itself is shared in
// Camera-Apple.mm; only device discovery differs from iOS.

namespace eacp::Cameras
{
NSArray<AVCaptureDeviceType>* platformDiscoveryDeviceTypes()
{
    auto* types = [NSMutableArray<AVCaptureDeviceType> array];
    [types addObject:AVCaptureDeviceTypeBuiltInWideAngleCamera];

    // External (USB) cameras are macOS-only, and the symbol needs the 14.0 SDK.
    if (@available(macOS 14.0, *))
        [types addObject:AVCaptureDeviceTypeExternal];

    return types;
}
} // namespace eacp::Cameras

#import <AVFoundation/AVFoundation.h>

#include "CameraDevices-Apple.h"

namespace eacp::Cameras
{
NSArray<AVCaptureDeviceType>* platformDiscoveryDeviceTypes()
{
    auto* types = [NSMutableArray<AVCaptureDeviceType> array];
    [types addObject:AVCaptureDeviceTypeBuiltInWideAngleCamera];

    // External (USB) cameras; the symbol needs the 14.0 SDK.
    if (@available(macOS 14.0, *))
        [types addObject:AVCaptureDeviceTypeExternal];

    return types;
}
} // namespace eacp::Cameras

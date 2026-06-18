#import <AVFoundation/AVFoundation.h>

#include "CameraDevices-Apple.h"

// iOS-specific camera pieces. The capture pipeline itself is shared in
// Camera-Apple.mm; only device discovery differs from macOS.

namespace eacp::Cameras
{
NSArray<AVCaptureDeviceType>* platformDiscoveryDeviceTypes()
{
    // The built-in lenses. Discovery with an unspecified position returns both
    // the front and back cameras for each available type; there are no external
    // cameras to enumerate as on macOS.
    auto* types = [NSMutableArray<AVCaptureDeviceType> array];
    [types addObject:AVCaptureDeviceTypeBuiltInWideAngleCamera];

    if (@available(iOS 13.0, *))
    {
        [types addObject:AVCaptureDeviceTypeBuiltInUltraWideCamera];
        [types addObject:AVCaptureDeviceTypeBuiltInTelephotoCamera];
    }

    return types;
}
} // namespace eacp::Cameras

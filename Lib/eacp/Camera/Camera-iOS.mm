#import <AVFoundation/AVFoundation.h>

#include "CameraDevices-Apple.h"

namespace eacp::Cameras
{
NSArray<AVCaptureDeviceType>* platformDiscoveryDeviceTypes()
{
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

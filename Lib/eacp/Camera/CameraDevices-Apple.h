#pragma once

#import <AVFoundation/AVFoundation.h>

namespace eacp::Cameras
{
// Defined per platform in Camera-macOS.mm / Camera-iOS.mm.
NSArray<AVCaptureDeviceType>* platformDiscoveryDeviceTypes();
} // namespace eacp::Cameras

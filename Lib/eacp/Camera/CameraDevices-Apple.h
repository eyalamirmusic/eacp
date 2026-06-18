#pragma once

#import <AVFoundation/AVFoundation.h>

// Internal seam between the shared Apple capture core (Camera-Apple.mm) and the
// per-platform pieces. macOS and iOS expose different camera hardware, so each
// provides its own device-type list for the discovery session.

namespace eacp::Cameras
{
// The AVCaptureDevice types the discovery session enumerates. Defined per
// platform in Camera-macOS.mm (built-in wide-angle plus external/USB cameras)
// and Camera-iOS.mm (the built-in front/back lenses).
NSArray<AVCaptureDeviceType>* platformDiscoveryDeviceTypes();
} // namespace eacp::Cameras

#include "Camera.h"

// Windows (Media Foundation) capture backend — implemented in a later phase.
// This stub keeps the cross-platform API linkable on Windows: enumeration
// returns nothing, permission reports NotDetermined and start() fails, so
// callers fall back gracefully until the real backend lands.

namespace eacp::Cameras
{
struct Camera::Native
{
};

Camera::Camera()
    : impl()
{
}

Camera::~Camera() = default;

Vector<CameraDevice> Camera::devices()
{
    return {};
}

Vector<CameraFormat> Camera::supportedFormats(const CameraDevice&)
{
    return {};
}

PermissionStatus Camera::permissionStatus()
{
    return PermissionStatus::NotDetermined;
}

void Camera::requestPermission(std::function<void(bool)> onResult)
{
    if (onResult)
        onResult(false);
}

void Camera::setFrameCallback(FrameCallback) {}

bool Camera::start(const CameraConfig&)
{
    return false;
}

void Camera::stop() {}

bool Camera::isRunning() const
{
    return false;
}

void* Camera::nativeSession() const
{
    return nullptr;
}
} // namespace eacp::Cameras

#pragma once

#include <eacp/Core/Utils/Common.h>
#include <eacp/Core/Utils/Containers.h>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>

namespace eacp::Graphics
{
class Image;
}

namespace eacp::Cameras
{
enum class PixelFormat
{
    BGRA8, // 32-bit BGRA, the byte order the capture path requests by default
    NV12 // planar 4:2:0; not yet delivered, reserved for a later phase
};

enum class PermissionStatus
{
    Granted,
    Denied,
    NotDetermined,
    Restricted
};

// A camera the system can capture from. id is a stable platform identifier
// (AVCaptureDevice.uniqueID on macOS) to pass back in CameraConfig::deviceId.
struct CameraDevice
{
    std::string id;
    std::string name;
    bool isFrontFacing = false;
};

// One capture mode a device supports — a resolution and the fastest frame rate
// it runs at. Reported by Camera::supportedFormats.
struct CameraFormat
{
    int width = 0;
    int height = 0;
    double maxFrameRate = 0.0;
    PixelFormat pixelFormat = PixelFormat::BGRA8;
};

// How to open the camera. An unset deviceId uses the system default camera.
// width/height/frameRate are advisory: the backend picks the closest mode the
// device supports.
struct CameraConfig
{
    std::optional<std::string> deviceId;
    int width = 1280;
    int height = 720;
    double frameRate = 30.0;
    PixelFormat pixelFormat = PixelFormat::BGRA8;
    bool discardLateFrames = true; // drop frames the consumer can't keep up with
};

// A single captured frame, handed to the frame callback. It is a lightweight
// non-owning view over the platform pixel buffer: data() and nativeBuffer() are
// valid only for the duration of the callback. To keep a frame past the call,
// copy it out (toImage) or retain the native buffer yourself.
class CameraFrame
{
public:
    CameraFrame(int width,
                int height,
                PixelFormat format,
                std::size_t bytesPerRow,
                double timestampSeconds,
                const std::uint8_t* data,
                void* nativeBuffer);

    int width() const { return frameWidth; }
    int height() const { return frameHeight; }
    PixelFormat format() const { return pixelFormat; }
    std::size_t bytesPerRow() const { return rowBytes; }
    double timestampSeconds() const { return timestamp; }

    // The raw pixel bytes (BGRA for PixelFormat::BGRA8), rows bytesPerRow apart
    // — which may exceed width * 4 when the row is padded. Null when the backend
    // could not map the buffer.
    const std::uint8_t* data() const { return pixels; }

    // The platform pixel buffer (CVPixelBufferRef on macOS) for zero-copy GPU
    // upload. Null on backends that don't expose one.
    void* nativeBuffer() const { return buffer; }

    // A tightly packed RGBA copy of the frame (top-left origin), ready for
    // Graphics::Image consumers and GPU upload. Returns an empty image when the
    // frame has no readable pixels. Defined in Camera.cpp (cross-platform).
    Graphics::Image toImage() const;

private:
    int frameWidth = 0;
    int frameHeight = 0;
    PixelFormat pixelFormat = PixelFormat::BGRA8;
    std::size_t rowBytes = 0;
    double timestamp = 0.0;
    const std::uint8_t* pixels = nullptr;
    void* buffer = nullptr;
};

// Invoked for each captured frame on a dedicated background capture thread (not
// the main thread). Keep the work short; offload heavy processing or marshal to
// the main thread (Threads::callAsync) as needed.
using FrameCallback = std::function<void(const CameraFrame&)>;

// A CPU-side copy of a frame for the upload display path (used on backends
// without a zero-copy native buffer, e.g. Windows) and other pull-based
// consumers. data is tightly packed BGRA8 (stride == width * 4). sequence bumps
// once per captured frame so a consumer can skip work when nothing is new.
struct FramePixels
{
    int width = 0;
    int height = 0;
    PixelFormat format = PixelFormat::BGRA8;
    Vector<std::uint8_t> data;
    std::uint64_t sequence = 0;
};

// Captures video from a camera. Raw frames go to the frame callback on a
// background thread; the same device feeds the display path (CameraView) via
// nativeSession(). One Camera drives one device at a time.
class Camera
{
public:
    Camera();
    ~Camera();

    Camera(const Camera&) = delete;
    Camera& operator=(const Camera&) = delete;

    // The cameras currently available to the system.
    static Vector<CameraDevice> devices();

    // The capture modes a device supports, or an empty list if it can't be
    // queried.
    static Vector<CameraFormat> supportedFormats(const CameraDevice& device);

    // The app's current camera authorisation. On macOS, enumeration works
    // regardless, but capture needs Granted.
    static PermissionStatus permissionStatus();

    // Asks the OS for camera access if it hasn't been decided yet. onResult is
    // delivered on the main thread with the granted/denied outcome.
    static void requestPermission(std::function<void(bool)> onResult);

    // Sets the per-frame callback. Set it before start(). Passing {} clears it.
    void setFrameCallback(FrameCallback callback);

    // Opens and configures the device, then starts the capture session on a
    // background thread (device warm-up can block). Returns false if the device
    // can't be opened or configured; otherwise frames begin arriving shortly
    // after — poll isRunning() or wait for the first frame. Check
    // permissionStatus() first, since capture needs Granted.
    bool start(const CameraConfig& config = {});
    void stop();
    bool isRunning() const;

    // The platform capture session (AVCaptureSession* on macOS), for the
    // display View to attach to without copying frames. Null when not running.
    void* nativeSession() const;

    // The most recent frame's native pixel buffer (CVPixelBufferRef on macOS),
    // retained — the caller owns the reference and must hand it back to
    // releasePixelBuffer. Null when no frame has arrived. Thread-safe; the
    // display path (CameraView) calls this on the render thread to wrap the
    // frame zero-copy, independently of the frame callback.
    void* acquireLatestPixelBuffer();

    // Releases a buffer returned by acquireLatestPixelBuffer.
    static void releasePixelBuffer(void* buffer);

    // Copies the most recent frame into `out` when it is newer than
    // out.sequence, reusing out's storage; returns true if a new frame was
    // copied, false if there is nothing newer. Thread-safe. The display path
    // uses this on backends without a zero-copy native buffer.
    bool copyLatestFrame(FramePixels& out);

private:
    struct Native;
    Pimpl<Native> impl;
};
} // namespace eacp::Cameras

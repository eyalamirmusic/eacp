#pragma once

#include <eacp/Core/Core.h>

namespace eacp::Graphics
{
class Image;
}

namespace eacp::Cameras
{
enum class PixelFormat
{
    BGRA8,
    NV12 // planar 4:2:0; not yet delivered
};

enum class PermissionStatus
{
    Granted,
    Denied,
    NotDetermined,
    Restricted
};

// id is a stable platform identifier (AVCaptureDevice.uniqueID on macOS).
struct CameraDevice
{
    std::string id;
    std::string name;
    bool isFrontFacing = false;
};

struct CameraFormat
{
    int width = 0;
    int height = 0;
    double maxFrameRate = 0.0;
    PixelFormat pixelFormat = PixelFormat::BGRA8;
};

// Unset deviceId uses the system default. width/height/frameRate are advisory:
// the backend picks the closest mode the device supports.
struct CameraConfig
{
    std::optional<std::string> deviceId;
    int width = 1280;
    int height = 720;
    double frameRate = 30.0;
    PixelFormat pixelFormat = PixelFormat::BGRA8;
    bool discardLateFrames = true;
};

// Non-owning view over the platform pixel buffer: data() and nativeBuffer() are
// valid only for the duration of the frame callback.
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

    // Rows are bytesPerRow apart, which may exceed width * 4 when padded. Null
    // when the backend could not map the buffer.
    const std::uint8_t* data() const { return pixels; }

    // CVPixelBufferRef on macOS, for zero-copy GPU upload. Null where the
    // backend doesn't expose one.
    void* nativeBuffer() const { return buffer; }

    // Tightly packed RGBA, row 0 is the top of the image. Empty when the frame
    // has no readable pixels.
    Graphics::Image toImage() const;

    // As above, recycling `reuse`'s storage; left empty on an unreadable frame.
    void toImage(Graphics::Image& reuse) const;

private:
    int frameWidth = 0;
    int frameHeight = 0;
    PixelFormat pixelFormat = PixelFormat::BGRA8;
    std::size_t rowBytes = 0;
    double timestamp = 0.0;
    const std::uint8_t* pixels = nullptr;
    void* buffer = nullptr;
};

// Invoked on the capture thread, never the main thread. Keep the work short.
using FrameCallback = std::function<void(const CameraFrame&)>;

// CPU-side frame copy for backends without a zero-copy native buffer. data is
// tightly packed BGRA8 (stride == width * 4); sequence bumps once per frame.
struct FramePixels
{
    int width = 0;
    int height = 0;
    PixelFormat format = PixelFormat::BGRA8;
    Vector<std::uint8_t> data;
    std::uint64_t sequence = 0;
};

// Drives one capture device at a time.
class Camera
{
public:
    Camera();
    ~Camera();

    Camera(const Camera&) = delete;
    Camera& operator=(const Camera&) = delete;

    static Vector<CameraDevice> devices();

    // Empty when the device can't be queried.
    static Vector<CameraFormat> supportedFormats(const CameraDevice& device);

    // Enumeration works regardless; capture needs Granted.
    static PermissionStatus permissionStatus();

    // onResult is delivered on the main thread.
    static void requestPermission(std::function<void(bool)> onResult);

    // Set before start(); passing {} clears it.
    void setFrameCallback(FrameCallback callback);

    // Invoked on the capture thread once each frame is stored as the latest, so
    // pair it with acquireLatestPixelBuffer or copyLatestFrame. One consumer at
    // a time, and unlike setFrameCallback may be reassigned while running.
    void setFrameArrivedCallback(Callback callback);

    // Configures the device, then starts the session on a background thread
    // (warm-up blocks). False if the device can't be opened; otherwise frames
    // begin arriving shortly after.
    bool start(const CameraConfig& config = {});
    void stop();
    bool isRunning() const;

    // AVCaptureSession* on macOS; null when not running.
    void* nativeSession() const;

    // Retained CVPixelBufferRef on macOS — the caller owns the reference and
    // must pass it to releasePixelBuffer. Null before the first frame.
    // Thread-safe; the render thread wraps it zero-copy.
    void* acquireLatestPixelBuffer();

    static void releasePixelBuffer(void* buffer);

    // Copies the latest frame into `out` when newer than out.sequence, reusing
    // its storage; false when there is nothing newer. Thread-safe.
    bool copyLatestFrame(FramePixels& out);

private:
    struct Native;
    Pimpl<Native> impl;
};
} // namespace eacp::Cameras

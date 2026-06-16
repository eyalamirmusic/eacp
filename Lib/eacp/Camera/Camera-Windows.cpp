#include <eacp/Core/Utils/WinInclude.h>

#include "Camera.h"

#include <eacp/Core/Threads/EventLoop.h>

#include <winrt/Windows.Devices.Enumeration.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Graphics.Imaging.h>
#include <winrt/Windows.Media.Capture.Frames.h>
#include <winrt/Windows.Media.Capture.h>
#include <winrt/Windows.Media.MediaProperties.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <mutex>
#include <thread>

// Windows capture backend (WinRT MediaCapture + MediaFrameReader). Frames arrive
// as CPU BGRA SoftwareBitmaps on a worker thread; the handler copies them into
// the latest-frame holder for the display path and hands a CameraFrame view to
// the user callback. MediaFrameReaderAcquisitionMode::Realtime drops late frames
// at the source (the backpressure). The session's WinRT objects live on a
// dedicated MTA worker thread for their whole lifetime.
//
// NOTE: authored to the documented MediaFrameReader pattern and the repo's WinRT
// conventions, but not yet compiled or run on Windows — expect a round of fixes
// on the first Windows/CI build. Display goes through a CPU copy
// (Texture::update); zero-copy via shared D3D11/D3D12 surfaces is a later phase.

// Classic-COM byte access to a SoftwareBitmap's locked buffer.
struct __declspec(uuid("5b0d3235-4dba-4d44-865e-8f1d0e4fd04d"))
IMemoryBufferByteAccess : ::IUnknown
{
    virtual HRESULT __stdcall GetBuffer(uint8_t** value, uint32_t* capacity) = 0;
};

namespace eacp::Cameras
{
namespace capture = winrt::Windows::Media::Capture;
namespace frames = winrt::Windows::Media::Capture::Frames;
namespace imaging = winrt::Windows::Graphics::Imaging;
namespace enumeration = winrt::Windows::Devices::Enumeration;
namespace mediaprops = winrt::Windows::Media::MediaProperties;

namespace
{
void ensureApartment()
{
    try
    {
        winrt::init_apartment(winrt::apartment_type::multi_threaded);
    }
    catch (const winrt::hresult_error&)
    {
    }
}

bool isUncompressedSubtype(const winrt::hstring& subtype)
{
    auto name = winrt::to_string(subtype);
    return _stricmp(name.c_str(), "NV12") == 0
           || _stricmp(name.c_str(), "YUY2") == 0;
}

// Color sources frequently default to a compressed subtype (e.g. MJPG), which
// the frame reader hands back as a raw buffer rather than a decoded
// SoftwareBitmap. Selecting an uncompressed mode closest to the requested size
// makes frames arrive as bitmaps the BGRA conversion can consume.
frames::MediaFrameFormat
    chooseUncompressedFormat(const frames::MediaFrameSource& source,
                             const CameraConfig& config)
{
    auto best = frames::MediaFrameFormat {nullptr};
    auto bestScore = std::numeric_limits<int>::max();

    for (auto const& format: source.SupportedFormats())
    {
        auto video = format.VideoFormat();

        if (video == nullptr || !isUncompressedSubtype(format.Subtype()))
            continue;

        auto score = std::abs((int) video.Width() - config.width)
                     + std::abs((int) video.Height() - config.height);

        if (score < bestScore)
        {
            bestScore = score;
            best = format;
        }
    }

    return best;
}
} // namespace

// CPU latest-frame holder: a tightly packed BGRA copy plus a sequence so the
// render thread only re-uploads when a new frame has arrived. The capture thread
// sets it; the render thread copies it out.
struct LatestFrame
{
    void set(const std::uint8_t* base, int width, int height, std::size_t stride)
    {
        std::lock_guard<std::mutex> lock(mutex);

        data.resize(width * height * 4);
        auto rowBytes = (std::size_t) width * 4;

        for (auto y = 0; y < height; ++y)
            std::memcpy(data.data() + (std::size_t) y * rowBytes,
                        base + (std::size_t) y * stride,
                        rowBytes);

        frameWidth = width;
        frameHeight = height;
        ++sequence;
    }

    bool copyInto(FramePixels& out)
    {
        std::lock_guard<std::mutex> lock(mutex);

        if (frameWidth <= 0 || sequence == out.sequence)
            return false;

        out.width = frameWidth;
        out.height = frameHeight;
        out.format = PixelFormat::BGRA8;
        out.data = data;
        out.sequence = sequence;
        return true;
    }

    std::mutex mutex;
    Vector<std::uint8_t> data;
    int frameWidth = 0;
    int frameHeight = 0;
    std::uint64_t sequence = 0;
};

struct CaptureContext
{
    FrameCallback callback;
    LatestFrame latest;
};

struct Camera::Native
{
    ~Native() { stop(); }

    void setFrameCallback(FrameCallback cb) { context.callback = std::move(cb); }

    bool start(const CameraConfig& config)
    {
        if (worker.joinable())
            return true;

        stopRequested.store(false);
        worker = std::thread([this, config] { runCapture(config); });
        return true;
    }

    void runCapture(const CameraConfig& config)
    {
        ensureApartment();

        try
        {
            mediaCapture = capture::MediaCapture();

            auto settings = capture::MediaCaptureInitializationSettings();
            settings.StreamingCaptureMode(capture::StreamingCaptureMode::Video);
            settings.MemoryPreference(capture::MediaCaptureMemoryPreference::Cpu);
            settings.SharingMode(capture::MediaCaptureSharingMode::ExclusiveControl);

            if (config.deviceId.has_value())
                settings.VideoDeviceId(winrt::to_hstring(*config.deviceId));

            mediaCapture.InitializeAsync(settings).get();

            auto colorSource = frames::MediaFrameSource {nullptr};

            for (auto const& entry: mediaCapture.FrameSources())
            {
                auto source = entry.Value();

                if (source.Info().SourceKind()
                    == frames::MediaFrameSourceKind::Color)
                {
                    colorSource = source;
                    break;
                }
            }

            if (!colorSource)
                return;

            if (auto format = chooseUncompressedFormat(colorSource, config))
                colorSource.SetFormatAsync(format).get();

            reader = mediaCapture.CreateFrameReaderAsync(colorSource).get();
            reader.AcquisitionMode(
                frames::MediaFrameReaderAcquisitionMode::Realtime);
            frameToken = reader.FrameArrived([this](auto const& sender, auto const&)
                                             { onFrame(sender); });

            if (reader.StartAsync().get()
                != frames::MediaFrameReaderStartStatus::Success)
            {
                running.store(false);
                reader = nullptr;
                mediaCapture = nullptr;
                return;
            }

            running.store(true);
        }
        catch (const winrt::hresult_error&)
        {
            running.store(false);
            reader = nullptr;
            mediaCapture = nullptr;
            return;
        }

        // Hold the WinRT objects on this MTA thread until stop() is requested.
        {
            std::unique_lock<std::mutex> lock(stopMutex);
            stopCondition.wait(lock, [this] { return stopRequested.load(); });
        }

        try
        {
            if (reader)
            {
                if (frameToken)
                    reader.FrameArrived(frameToken);

                reader.StopAsync().get();
            }
        }
        catch (const winrt::hresult_error&)
        {
        }

        reader = nullptr;
        mediaCapture = nullptr;
        running.store(false);
    }

    void onFrame(const frames::MediaFrameReader& sender)
    {
        auto frame = sender.TryAcquireLatestFrame();

        if (!frame)
            return;

        auto videoFrame = frame.VideoMediaFrame();

        if (!videoFrame)
            return;

        auto bitmap = videoFrame.SoftwareBitmap();

        if (!bitmap)
            return;

        if (bitmap.BitmapPixelFormat() != imaging::BitmapPixelFormat::Bgra8)
            bitmap = imaging::SoftwareBitmap::Convert(
                bitmap, imaging::BitmapPixelFormat::Bgra8);

        auto width = bitmap.PixelWidth();
        auto height = bitmap.PixelHeight();

        auto buffer = bitmap.LockBuffer(imaging::BitmapBufferAccessMode::Read);
        auto plane = buffer.GetPlaneDescription(0);
        auto reference = buffer.CreateReference();

        std::uint8_t* bytes = nullptr;
        std::uint32_t capacity = 0;
        auto access = reference.as<IMemoryBufferByteAccess>();

        if (FAILED(access->GetBuffer(&bytes, &capacity)) || bytes == nullptr)
            return;

        const auto* base = bytes + plane.StartIndex;
        auto stride = (std::size_t) plane.Stride;

        auto timestampSeconds = 0.0;
        auto relative = frame.SystemRelativeTime();

        if (relative)
            timestampSeconds =
                std::chrono::duration_cast<std::chrono::duration<double>>(
                    relative.Value())
                    .count();

        context.latest.set(base, width, height, stride);

        if (context.callback)
        {
            auto cameraFrame = CameraFrame(width,
                                           height,
                                           PixelFormat::BGRA8,
                                           stride,
                                           timestampSeconds,
                                           base,
                                           nullptr);
            context.callback(cameraFrame);
        }
    }

    void stop()
    {
        if (!worker.joinable())
            return;

        {
            std::lock_guard<std::mutex> lock(stopMutex);
            stopRequested.store(true);
        }

        stopCondition.notify_all();
        worker.join();
        running.store(false);
    }

    CaptureContext context;
    capture::MediaCapture mediaCapture {nullptr};
    frames::MediaFrameReader reader {nullptr};
    winrt::event_token frameToken {};
    std::thread worker;
    std::mutex stopMutex;
    std::condition_variable stopCondition;
    std::atomic<bool> running {false};
    std::atomic<bool> stopRequested {false};
};

Camera::Camera()
    : impl()
{
}

Camera::~Camera() = default;

Vector<CameraDevice> Camera::devices()
{
    ensureApartment();

    auto result = Vector<CameraDevice> {};

    try
    {
        auto found = enumeration::DeviceInformation::FindAllAsync(
                         enumeration::DeviceClass::VideoCapture)
                         .get();

        for (auto const& device: found)
        {
            auto info = CameraDevice {};
            info.id = winrt::to_string(device.Id());
            info.name = winrt::to_string(device.Name());
            info.isFrontFacing = false;
            result.push_back(std::move(info));
        }
    }
    catch (const winrt::hresult_error&)
    {
    }

    return result;
}

Vector<CameraFormat> Camera::supportedFormats(const CameraDevice&)
{
    // Querying modes needs a MediaCapture session per device; not wired up yet.
    return {};
}

PermissionStatus Camera::permissionStatus()
{
    // Desktop Win32 has no pre-flight query; the global privacy setting is
    // enforced when InitializeAsync runs, so start() fails if access is denied.
    return PermissionStatus::Granted;
}

void Camera::requestPermission(std::function<void(bool)> onResult)
{
    Threads::callAsync(
        [onResult]
        {
            if (onResult)
                onResult(true);
        });
}

void Camera::setFrameCallback(FrameCallback callback)
{
    impl->setFrameCallback(std::move(callback));
}

bool Camera::start(const CameraConfig& config)
{
    return impl->start(config);
}

void Camera::stop()
{
    impl->stop();
}

bool Camera::isRunning() const
{
    return impl->running.load();
}

void* Camera::nativeSession() const
{
    return nullptr;
}

void* Camera::acquireLatestPixelBuffer()
{
    return nullptr;
}

void Camera::releasePixelBuffer(void*) {}

bool Camera::copyLatestFrame(FramePixels& out)
{
    return impl->context.latest.copyInto(out);
}
} // namespace eacp::Cameras

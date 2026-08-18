#include <eacp/Core/Utils/WinInclude.h>

#include "Encoder-Windows.h"
#include "ScreenCapture.h"

#include <eacp/Core/Threads/EventLoop.h>
#include <eacp/Core/Utils/Logging.h>
#include <eacp/Graphics/Window/CompositionHostWindow-Windows.h>

#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Graphics.Capture.h>
#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>
#include <winrt/Windows.Graphics.h>

#include <Windows.Graphics.Capture.Interop.h>
#include <windows.graphics.directx.direct3d11.interop.h>

#include <d3d11_4.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <mutex>

// The Screen tier on Windows: Windows.Graphics.Capture taps the compositor's
// live composite of the view's host window (2D, GPU and WebView together) and
// hands it over as D3D11 textures, which are read back through a staging
// surface and fed to the shared Media Foundation encoder. Real-time, and --
// unlike ScreenCaptureKit -- with no permission gate: CreateForWindow captures
// the app's own window without the system picker.

namespace eacp::Video
{
namespace
{
namespace capture = winrt::Windows::Graphics::Capture;
namespace directx = winrt::Windows::Graphics::DirectX;
namespace direct3d = winrt::Windows::Graphics::DirectX::Direct3D11;

using Microsoft::WRL::ComPtr;

// Deep enough to absorb a slow encoder without the compositor dropping the
// window's frames, shallow enough that a stale one is never far behind.
constexpr auto framePoolBuffers = 2;

constexpr auto capturePixelFormat =
    directx::DirectXPixelFormat::B8G8R8A8UIntNormalized;

int roundDownToEvenPixels(std::int32_t value)
{
    return static_cast<int>(value) & ~1;
}

// Direct3D11CaptureFrame::SystemRelativeTime is QPC time in 100ns ticks, so
// the recorder's origin is read off the same clock.
double qpcSeconds()
{
    auto frequency = LARGE_INTEGER {};
    auto counter = LARGE_INTEGER {};
    QueryPerformanceFrequency(&frequency);
    QueryPerformanceCounter(&counter);

    return static_cast<double>(counter.QuadPart)
           / static_cast<double>(frequency.QuadPart);
}

double frameSeconds(const capture::Direct3D11CaptureFrame& frame)
{
    return std::chrono::duration<double>(frame.SystemRelativeTime()).count();
}

// WGC is a WinRT API, and the thread it is started from has to be in an
// apartment. The GUI thread usually already is -- a matching re-init is a
// no-op, and a conflicting one (MTA) throws and is equally fine to capture
// from, so both outcomes carry on.
void ensureWinRtApartment()
{
    try
    {
        winrt::init_apartment(winrt::apartment_type::single_threaded);
    }
    catch (const winrt::hresult_error&)
    {
    }
}

capture::GraphicsCaptureItem createItemForWindow(HWND window)
{
    auto item = capture::GraphicsCaptureItem {nullptr};

    auto interop = winrt::get_activation_factory<capture::GraphicsCaptureItem,
                                                 ::IGraphicsCaptureItemInterop>();

    if (FAILED(
            interop->CreateForWindow(window,
                                     winrt::guid_of<capture::GraphicsCaptureItem>(),
                                     winrt::put_abi(item))))
        return nullptr;

    return item;
}

struct WindowsScreenCapture final : ScreenCapture
{
    ~WindowsScreenCapture() override { closeCapture(); }

    bool start(Graphics::View& view,
               const FilePath& path,
               const RecordingOptions& options,
               Encoder& encoderToUse) override
    {
        if (!capture::GraphicsCaptureSession::IsSupported())
        {
            LOG("VideoRecorder: Windows.Graphics.Capture is not available here");
            return false;
        }

        auto hostWindow = Graphics::findHostHwndForView(&view);

        if (hostWindow == nullptr)
        {
            LOG("VideoRecorder: view has no host window to screen-capture");
            return false;
        }

        if (IsIconic(hostWindow))
        {
            LOG("VideoRecorder: host window is minimized; nothing to capture");
            return false;
        }

        ensureWinRtApartment();

        auto item = createItemForWindow(hostWindow);

        if (item == nullptr)
        {
            LOG("VideoRecorder: could not open the host window for capture");
            return false;
        }

        // The compositor hands back physical pixels, so the window's own size is
        // the recording size -- RecordingOptions::scale has nothing to resample
        // here and is ignored, unlike the off-screen tiers that render at it.
        poolSize = item.Size();
        width = roundDownToEvenPixels(poolSize.Width);
        height = roundDownToEvenPixels(poolSize.Height);

        if (width <= 0 || height <= 0)
        {
            LOG("VideoRecorder: host window has no capturable size");
            return false;
        }

        if (!createCaptureDevice())
        {
            LOG("VideoRecorder: no D3D11 device for screen capture");
            return false;
        }

        encoder = static_cast<WindowsEncoder*>(&encoderToUse);

        if (!encoder->begin(path, specFor(options)))
        {
            LOG("VideoRecorder: encoder setup failed");
            return false;
        }

        // The recorder's timeline starts here, so the QPC stamps the compositor
        // hands back are rebased onto it -- which is what puts them on the same
        // zero as the audio the app pushes.
        timelineStart = qpcSeconds();
        frameInterval = options.fps > 0 ? 1.0 / options.fps : 0.0;
        nextCapture = 0.0;
        active = true;

        framePool = capture::Direct3D11CaptureFramePool::CreateFreeThreaded(
            device, capturePixelFormat, framePoolBuffers, poolSize);

        frameToken =
            framePool.FrameArrived({this, &WindowsScreenCapture::onFrameArrived});

        session = framePool.CreateCaptureSession(item);
        applySessionOptions();
        session.StartCapture();

        LOG("VideoRecorder: screen capture started");
        return true;
    }

    Threads::Async<void> stop() override
    {
        {
            auto lock = std::lock_guard {frameMutex};
            active = false;
        }

        // Revoking waits for a handler that is already running, so the flag has
        // to be clear before this -- and the mutex released, or the handler
        // waiting on it and the revoke waiting on the handler would deadlock.
        closeCapture();

        if (encoder == nullptr)
        {
            auto promise = Threads::AsyncPromise<void> {};
            promise.resolve();
            return promise.get();
        }

        return encoder->finish();
    }

    EncoderSpec specFor(const RecordingOptions& options) const
    {
        auto spec = EncoderSpec {};
        spec.video.width = width;
        spec.video.height = height;
        spec.video.fps = options.fps > 0 ? options.fps : 60;
        spec.video.bitrate =
            options.bitrate > 0 ? options.bitrate : width * height * 8;
        spec.audio = options.audio;
        return spec;
    }

    bool createCaptureDevice()
    {
        // The capture runs on its own device rather than the compositor's: that
        // one is rebuilt from under its holders on device loss (see
        // DComp-Windows.h), which would take the frame pool with it.
        auto create = [this](D3D_DRIVER_TYPE driver)
        {
            return D3D11CreateDevice(nullptr,
                                     driver,
                                     nullptr,
                                     D3D11_CREATE_DEVICE_BGRA_SUPPORT,
                                     nullptr,
                                     0,
                                     D3D11_SDK_VERSION,
                                     &d3dDevice,
                                     nullptr,
                                     &context);
        };

        if (FAILED(create(D3D_DRIVER_TYPE_HARDWARE))
            && FAILED(create(D3D_DRIVER_TYPE_WARP)))
            return false;

        // Frames arrive on a thread pool while start() and stop() run on the
        // main thread, so the immediate context has to serialise itself.
        ComPtr<ID3D11Multithread> multithread;
        if (SUCCEEDED(context.As(&multithread)))
            multithread->SetMultithreadProtected(TRUE);

        ComPtr<IDXGIDevice> dxgiDevice;
        if (FAILED(d3dDevice.As(&dxgiDevice)))
            return false;

        auto inspectable = winrt::com_ptr<::IInspectable> {};
        if (FAILED(CreateDirect3D11DeviceFromDXGIDevice(dxgiDevice.Get(),
                                                        inspectable.put())))
            return false;

        device = inspectable.as<direct3d::IDirect3DDevice>();
        return device != nullptr;
    }

    // Both are newer than the API itself and throw where the OS does not have
    // them: the cursor stays out of the file from Windows 10 2004, and the
    // yellow capture border goes away on Windows 11 -- older builds record with
    // it, which is cosmetic and not worth failing the recording over.
    void applySessionOptions()
    {
        try
        {
            session.IsCursorCaptureEnabled(false);
        }
        catch (const winrt::hresult_error&)
        {
        }

        try
        {
            session.IsBorderRequired(false);
        }
        catch (const winrt::hresult_error&)
        {
        }
    }

    void closeCapture()
    {
        if (session != nullptr)
        {
            session.Close();
            session = nullptr;
        }

        if (framePool != nullptr)
        {
            framePool.FrameArrived(frameToken);
            framePool.Close();
            framePool = nullptr;
        }
    }

    // Holds the target frame rate against a compositor presenting faster:
    // returns true only when the next scheduled slot is due, advancing on an
    // ideal grid so it does not drift.
    bool paceAllows(double time)
    {
        if (frameInterval <= 0.0)
            return true;

        if (time < nextCapture)
            return false;

        nextCapture += frameInterval;
        if (nextCapture <= time)
            nextCapture = time + frameInterval;

        return true;
    }

    void onFrameArrived(const capture::Direct3D11CaptureFramePool& sender,
                        const winrt::Windows::Foundation::IInspectable&)
    {
        auto lock = std::lock_guard {frameMutex};

        if (!active)
            return;

        auto frame = sender.TryGetNextFrame();

        if (frame == nullptr)
            return;

        auto contentSize = frame.ContentSize();

        // A frame stamped fractionally before the timeline opened belongs at
        // its start, not at a negative time the writer would refuse.
        auto pts = std::max(0.0, frameSeconds(frame) - timelineStart);

        if (paceAllows(pts))
            appendFrame(frame, contentSize, pts);

        frame.Close();

        // The window was resized: the pool follows it, while the file keeps the
        // size it was opened with and the encoder crops or pads into it.
        if (contentSize.Width != poolSize.Width
            || contentSize.Height != poolSize.Height)
        {
            poolSize = contentSize;
            staging.Reset();
            framePool.Recreate(
                device, capturePixelFormat, framePoolBuffers, poolSize);
        }
    }

    void appendFrame(const capture::Direct3D11CaptureFrame& frame,
                     winrt::Windows::Graphics::SizeInt32 contentSize,
                     double pts)
    {
        auto access = frame.Surface()
                          .as<::Windows::Graphics::DirectX::Direct3D11::
                                  IDirect3DDxgiInterfaceAccess>();

        ComPtr<ID3D11Texture2D> texture;
        if (FAILED(access->GetInterface(IID_PPV_ARGS(&texture))))
            return;

        auto description = D3D11_TEXTURE2D_DESC {};
        texture->GetDesc(&description);

        if (!ensureStaging(description))
            return;

        context->CopyResource(staging.Get(), texture.Get());

        auto mapped = D3D11_MAPPED_SUBRESOURCE {};
        if (FAILED(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped)))
            return;

        // The pool's texture can be larger than the window currently draws to,
        // so the content size is what carries real pixels.
        encoder->appendBGRA(static_cast<const std::uint8_t*>(mapped.pData),
                            std::min(contentSize.Width,
                                     static_cast<std::int32_t>(description.Width)),
                            std::min(contentSize.Height,
                                     static_cast<std::int32_t>(description.Height)),
                            mapped.RowPitch,
                            pts);

        context->Unmap(staging.Get(), 0);
    }

    // The CPU-readable twin of the pool's texture. Rebuilt whenever the window
    // resizes; the zero-copy path (a DXGI-backed MF sample) would drop it, but
    // that needs the sink writer bound to this device and an NV12 input type.
    bool ensureStaging(const D3D11_TEXTURE2D_DESC& source)
    {
        if (staging != nullptr && stagingWidth == source.Width
            && stagingHeight == source.Height)
            return true;

        staging.Reset();

        auto description = D3D11_TEXTURE2D_DESC {};
        description.Width = source.Width;
        description.Height = source.Height;
        description.MipLevels = 1;
        description.ArraySize = 1;
        description.Format = source.Format;
        description.SampleDesc.Count = 1;
        description.Usage = D3D11_USAGE_STAGING;
        description.CPUAccessFlags = D3D11_CPU_ACCESS_READ;

        if (FAILED(d3dDevice->CreateTexture2D(&description, nullptr, &staging)))
            return false;

        stagingWidth = source.Width;
        stagingHeight = source.Height;
        return true;
    }

    ComPtr<ID3D11Device> d3dDevice;
    ComPtr<ID3D11DeviceContext> context;
    ComPtr<ID3D11Texture2D> staging;
    UINT stagingWidth = 0;
    UINT stagingHeight = 0;

    direct3d::IDirect3DDevice device {nullptr};
    capture::Direct3D11CaptureFramePool framePool {nullptr};
    capture::GraphicsCaptureSession session {nullptr};
    winrt::event_token frameToken;
    winrt::Windows::Graphics::SizeInt32 poolSize {};

    WindowsEncoder* encoder = nullptr;

    // Held for the whole of a frame's handling, so stop() knows no frame is in
    // flight once it has the lock.
    std::mutex frameMutex;
    bool active = false;

    double timelineStart = 0.0;
    double frameInterval = 0.0;
    double nextCapture = 0.0;
    int width = 0;
    int height = 0;
};
} // namespace

// Windows.Graphics.Capture needs no user consent to capture the app's own
// window -- the picker exists for capturing somebody else's -- so there is
// nothing to ask for and nothing that can refuse.
bool hasScreenCapturePermission()
{
    return true;
}

void requestScreenCapturePermission(std::function<void(bool)> onResult)
{
    Threads::callAsync(
        [onResult]
        {
            if (onResult)
                onResult(true);
        });
}

OwningPointer<ScreenCapture> makeScreenCapture()
{
    return makeOwned<WindowsScreenCapture>();
}

} // namespace eacp::Video

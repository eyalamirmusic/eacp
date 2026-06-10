#include <eacp/Core/Utils/WinInclude.h>
#include <eacp/Core/Utils/Containers.h>

#include <functional>

#include <d3d11.h>
#include <dxgi1_2.h>
#include <d2d1_1.h>
#include <dwrite.h>

#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.UI.Composition.h>
#include <windows.ui.composition.interop.h>

namespace wuc = winrt::Windows::UI::Composition;

namespace eacp::Graphics
{

// Defined in CompositionHostWindow-Windows.cpp: re-renders every layer and
// repaints every painting view of all composition-hosted windows. Needed after
// the rendering device is replaced, which keeps the composition surfaces alive
// but discards their contents.
void redrawAllCompositionHosts();

namespace
{
Vector<std::function<void()>>& renderingDeviceReplacedListeners()
{
    static auto listeners = Vector<std::function<void()>> {};
    return listeners;
}

// Everything that has to happen once a replacement rendering device is live:
// re-render the 2D composition content, then let other modules (the GPU
// device and its swapchains) re-acquire. May run twice for one loss — once
// directly from recovery, once from RenderingDeviceReplaced — so listeners
// must tolerate being called with an unchanged device.
void handleRenderingDeviceReplaced()
{
    redrawAllCompositionHosts();

    for (auto& listener: renderingDeviceReplacedListeners())
        listener();
}
} // namespace

// Registers a callback for after the shared D3D/D2D device was replaced
// following device loss. Used by eacp-gpu to re-acquire the device and rebuild
// swapchains. Listeners are never unregistered, so only register from
// process-lifetime objects. Main-thread only.
void addRenderingDeviceReplacedListener(std::function<void()> listener)
{
    renderingDeviceReplacedListeners().add(std::move(listener));
}

class WinRTCompositor
{
public:
    static WinRTCompositor& instance()
    {
        static auto instance = WinRTCompositor();
        return instance;
    }

    bool recoverFromDeviceLoss(HRESULT hr)
    {
        if (hr != DXGI_ERROR_DEVICE_REMOVED && hr != DXGI_ERROR_DEVICE_RESET
            && hr != D2DERR_RECREATE_TARGET)
            return false;

        return recreateRenderingDevice();
    }

    ID2D1Factory1* getD2DFactory() const { return d2dFactory.get(); }
    IDWriteFactory* getDWriteFactory() const { return dwriteFactory.get(); }
    ID3D11Device* getD3DDevice() const { return d3dDevice.get(); }
    IDXGIDevice* getDXGIDevice() const { return dxgiDevice.get(); }
    ID2D1Device* getD2DDevice() const { return d2dDevice.get(); }
    wuc::Compositor getCompositor() { return compositor; }
    wuc::CompositionGraphicsDevice getGraphicsDevice() { return graphicsDevice; }

    bool isInitialized() const { return initialized; }

private:
    // The COM apartment (STA) and the thread's DispatcherQueue are owned by
    // initLoopThread() in EventLoop-Windows.cpp, which runs before any app —
    // and therefore any graphics — code. Owning neither here means this
    // singleton's static destructor releases plain COM references at exit and
    // never tears the apartment down under other statics (WebView2, WinRT
    // factory caches) that still have to release.
    WinRTCompositor()
    {
        winrt::check_hresult(
            DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED,
                                __uuidof(IDWriteFactory),
                                reinterpret_cast<IUnknown**>(dwriteFactory.put())));

        winrt::check_hresult(
            D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, d2dFactory.put()));

        createRenderingDevice();

        compositor = wuc::Compositor();

        namespace Interop = ABI::Windows::UI::Composition;
        auto interop = compositor.as<Interop::ICompositorInterop>();
        winrt::com_ptr<Interop::ICompositionGraphicsDevice> abiDevice;
        winrt::check_hresult(
            interop->CreateGraphicsDevice(d2dDevice.get(), abiDevice.put()));

        graphicsDevice = abiDevice.as<wuc::CompositionGraphicsDevice>();

        // Fires after SetRenderingDevice below, and also when the system
        // replaces the device on its own (driver update, GPU reset). Surfaces
        // survive the swap but lose their pixels, so everything re-renders.
        graphicsDevice.RenderingDeviceReplaced([](auto&&, auto&&)
                                               { handleRenderingDeviceReplaced(); });

        initialized = true;
    }

    // Creates (or re-creates, after device loss) the D3D + D2D device pair the
    // factories stay independent of.
    void createRenderingDevice()
    {
        d2dDevice = nullptr;
        dxgiDevice = nullptr;
        d3dDevice = nullptr;

        // BGRA support is required for D2D interop.
        Array featureLevels = {D3D_FEATURE_LEVEL_11_1,
                               D3D_FEATURE_LEVEL_11_0,
                               D3D_FEATURE_LEVEL_10_1,
                               D3D_FEATURE_LEVEL_10_0};
        D3D_FEATURE_LEVEL featureLevel;

        // No SINGLETHREADED: the device is shared with the composition engine
        // (CompositionGraphicsDevice), which may touch it off-thread.
        auto hr = D3D11CreateDevice(nullptr,
                                    D3D_DRIVER_TYPE_HARDWARE,
                                    nullptr,
                                    D3D11_CREATE_DEVICE_BGRA_SUPPORT,
                                    featureLevels.data(),
                                    static_cast<UINT>(featureLevels.size()),
                                    D3D11_SDK_VERSION,
                                    d3dDevice.put(),
                                    &featureLevel,
                                    nullptr);

        if (FAILED(hr))
        {
            // Fallback to WARP software renderer
            winrt::check_hresult(
                D3D11CreateDevice(nullptr,
                                  D3D_DRIVER_TYPE_WARP,
                                  nullptr,
                                  D3D11_CREATE_DEVICE_BGRA_SUPPORT,
                                  featureLevels.data(),
                                  static_cast<UINT>(featureLevels.size()),
                                  D3D11_SDK_VERSION,
                                  d3dDevice.put(),
                                  &featureLevel,
                                  nullptr));
        }

        dxgiDevice = d3dDevice.as<IDXGIDevice>();

        winrt::check_hresult(
            d2dFactory->CreateDevice(dxgiDevice.get(), d2dDevice.put()));
    }

    bool recreateRenderingDevice()
    {
        try
        {
            createRenderingDevice();

            namespace Interop = ABI::Windows::UI::Composition;
            auto interop =
                graphicsDevice.as<Interop::ICompositionGraphicsDeviceInterop>();
            winrt::check_hresult(interop->SetRenderingDevice(d2dDevice.get()));

            // RenderingDeviceReplaced notifies too, but it can arrive
            // asynchronously; notify directly so recovery doesn't depend on
            // event delivery.
            handleRenderingDeviceReplaced();
            return true;
        }
        catch (const winrt::hresult_error&)
        {
            // The GPU may still be resetting; the next BeginDraw failure
            // retries recovery.
            return false;
        }
    }

    ~WinRTCompositor()
    {
        graphicsDevice = nullptr;
        compositor = nullptr;
        d2dDevice = nullptr;
        d2dFactory = nullptr;
        dxgiDevice = nullptr;
        d3dDevice = nullptr;
        dwriteFactory = nullptr;
    }

    bool initialized = false;
    winrt::com_ptr<ID3D11Device> d3dDevice;
    winrt::com_ptr<IDXGIDevice> dxgiDevice;
    winrt::com_ptr<ID2D1Factory1> d2dFactory;
    winrt::com_ptr<ID2D1Device> d2dDevice;
    winrt::com_ptr<IDWriteFactory> dwriteFactory;
    wuc::Compositor compositor {nullptr};
    wuc::CompositionGraphicsDevice graphicsDevice {nullptr};
};

ID2D1Factory1* getD2DFactory()
{
    return WinRTCompositor::instance().getD2DFactory();
}

IDWriteFactory* getDWriteFactory()
{
    return WinRTCompositor::instance().getDWriteFactory();
}

ID3D11Device* getD3DDevice()
{
    return WinRTCompositor::instance().getD3DDevice();
}

IDXGIDevice* getDXGIDevice()
{
    return WinRTCompositor::instance().getDXGIDevice();
}

ID2D1Device* getD2DDevice()
{
    return WinRTCompositor::instance().getD2DDevice();
}

wuc::Compositor getWinRTCompositor()
{
    return WinRTCompositor::instance().getCompositor();
}

wuc::CompositionGraphicsDevice getCompositionGraphicsDevice()
{
    return WinRTCompositor::instance().getGraphicsDevice();
}

bool isCompositorInitialized()
{
    return WinRTCompositor::instance().isInitialized();
}

bool handleDeviceLossIfNeeded(HRESULT hr)
{
    return WinRTCompositor::instance().recoverFromDeviceLoss(hr);
}

} // namespace eacp::Graphics

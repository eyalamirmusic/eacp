#include <cstdio>

#include "../DComp-Windows.h"

#include "../Common.h"

#include <d3d11.h>
#include <dxgi1_2.h>

namespace eacp::Graphics
{

// Defined in CompositionHostWindow-Windows.cpp. DirectComposition binds its
// rendering device at creation and cannot swap it, so a lost device invalidates
// every target, visual and surface — the hosts rebuild theirs, then redraw.
// (Windows.UI.Composition could hot-swap the device via SetRenderingDevice and
// only had to repaint; DComp has no equivalent.)
void rebuildAllCompositionHosts();

namespace
{
Vector<std::function<void()>>& renderingDeviceReplacedListeners()
{
    static auto listeners = Vector<std::function<void()>> {};
    return listeners;
}

// Everything that has to happen once a replacement rendering device is live:
// rebuild and re-render the 2D composition content, then let other modules (the
// GPU device and its swapchains) re-acquire. Listeners must tolerate being
// called with an unchanged device.
void handleRenderingDeviceReplaced()
{
    rebuildAllCompositionHosts();

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

class DCompCompositor
{
public:
    static DCompCompositor& instance()
    {
        static auto instance = DCompCompositor();
        return instance;
    }

    bool recoverFromDeviceLoss(HRESULT hr)
    {
        if (hr != DXGI_ERROR_DEVICE_REMOVED && hr != DXGI_ERROR_DEVICE_RESET
            && hr != D2DERR_RECREATE_TARGET)
            return false;

        return recreateRenderingDevice();
    }

    ID2D1Factory1* getD2DFactory() const { return d2dFactory.Get(); }
    IDWriteFactory* getDWriteFactory() const { return dwriteFactory.Get(); }
    ID3D11Device* getD3DDevice() const { return d3dDevice.Get(); }
    IDXGIDevice* getDXGIDevice() const { return dxgiDevice.Get(); }
    ID2D1Device* getD2DDevice() const { return d2dDevice.Get(); }
    IDCompositionDesktopDevice* getDevice() const { return device.Get(); }

    uint64_t getGeneration() const { return generation; }
    bool isInitialized() const { return initialized; }

    // A failed Commit is itself a device-loss signal, so route it through
    // recovery rather than dropping it: otherwise the tree silently stops
    // updating until something else happens to fail.
    void commit()
    {
        if (!device)
            return;

        if (auto hr = device->Commit(); FAILED(hr))
            recoverFromDeviceLoss(hr);
    }

private:
    // A headless or hosted context — most notably a plugin editor opened on a
    // CI runner or in a session with no accessible desktop compositor — can fail
    // to create the D3D / DComp device, typically with E_ACCESSDENIED. Every
    // consumer already guards a null device and degrades to no GPU compositing,
    // so failure must NOT escape into a host (a DAW, a validator) that installed
    // no handler. Log it and leave the singleton uninitialised instead.
    DCompCompositor()
    {
        if (auto hr = create(); FAILED(hr))
        {
            char code[16];
            std::snprintf(
                code, sizeof code, "0x%08lx", static_cast<unsigned long>(hr));
            LOG("DCompCompositor: composition device init failed (hr=",
                code,
                "); GPU compositing unavailable. Expected on headless sessions "
                "and plugin hosts without an accessible desktop compositor — "
                "views degrade to no compositing instead of crashing the host.");

            releaseAll();
            return;
        }

        initialized = true;
    }

    ~DCompCompositor() { releaseAll(); }

    HRESULT create()
    {
        auto hr = DWriteCreateFactory(
            DWRITE_FACTORY_TYPE_SHARED,
            __uuidof(IDWriteFactory),
            reinterpret_cast<IUnknown**>(dwriteFactory.GetAddressOf()));
        if (FAILED(hr))
            return hr;

        hr = D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED,
                               d2dFactory.GetAddressOf());
        if (FAILED(hr))
            return hr;

        hr = createRenderingDevice();
        if (FAILED(hr))
            return hr;

        return createCompositionDevice();
    }

    // Creates (or re-creates, after device loss) the D3D + D2D device pair the
    // factories stay independent of.
    HRESULT createRenderingDevice()
    {
        d2dDevice.Reset();
        dxgiDevice.Reset();
        d3dDevice.Reset();

        // BGRA support is required for D2D interop.
        Array featureLevels = {D3D_FEATURE_LEVEL_11_1,
                               D3D_FEATURE_LEVEL_11_0,
                               D3D_FEATURE_LEVEL_10_1,
                               D3D_FEATURE_LEVEL_10_0};
        D3D_FEATURE_LEVEL featureLevel;

        // No SINGLETHREADED: the device is shared with the composition engine,
        // which may touch it off-thread.
        auto hr = D3D11CreateDevice(nullptr,
                                    D3D_DRIVER_TYPE_HARDWARE,
                                    nullptr,
                                    D3D11_CREATE_DEVICE_BGRA_SUPPORT,
                                    featureLevels.data(),
                                    static_cast<UINT>(featureLevels.size()),
                                    D3D11_SDK_VERSION,
                                    d3dDevice.GetAddressOf(),
                                    &featureLevel,
                                    nullptr);

        if (FAILED(hr))
        {
            // Fallback to WARP software renderer
            hr = D3D11CreateDevice(nullptr,
                                   D3D_DRIVER_TYPE_WARP,
                                   nullptr,
                                   D3D11_CREATE_DEVICE_BGRA_SUPPORT,
                                   featureLevels.data(),
                                   static_cast<UINT>(featureLevels.size()),
                                   D3D11_SDK_VERSION,
                                   d3dDevice.GetAddressOf(),
                                   &featureLevel,
                                   nullptr);
        }

        if (FAILED(hr))
            return hr;

        hr = d3dDevice.As(&dxgiDevice);
        if (FAILED(hr))
            return hr;

        return d2dFactory->CreateDevice(dxgiDevice.Get(), d2dDevice.GetAddressOf());
    }

    // The DComp device takes the D2D device straight in — no separate
    // CompositionGraphicsDevice, and no ICompositorInterop dance to build one.
    HRESULT createCompositionDevice()
    {
        device.Reset();

        return DCompositionCreateDevice2(d2dDevice.Get(),
                                         IID_PPV_ARGS(device.GetAddressOf()));
    }

    bool recreateRenderingDevice()
    {
        if (FAILED(createRenderingDevice()) || FAILED(createCompositionDevice()))
        {
            // The GPU may still be resetting; the next BeginDraw failure retries.
            releaseAll();
            initialized = false;
            return false;
        }

        initialized = true;

        // Everything built against the old device — targets, visuals, surfaces —
        // is dead. Moving the generation is what tells every holder to rebuild.
        ++generation;
        handleRenderingDeviceReplaced();

        return true;
    }

    void releaseAll()
    {
        device.Reset();
        d2dDevice.Reset();
        d2dFactory.Reset();
        dxgiDevice.Reset();
        d3dDevice.Reset();
        dwriteFactory.Reset();
    }

    bool initialized = false;
    uint64_t generation = 1;

    ComPtr<ID3D11Device> d3dDevice;
    ComPtr<IDXGIDevice> dxgiDevice;
    ComPtr<ID2D1Factory1> d2dFactory;
    ComPtr<ID2D1Device> d2dDevice;
    ComPtr<IDWriteFactory> dwriteFactory;
    ComPtr<IDCompositionDesktopDevice> device;
};

ID2D1Factory1* getD2DFactory()
{
    return DCompCompositor::instance().getD2DFactory();
}

IDWriteFactory* getDWriteFactory()
{
    return DCompCompositor::instance().getDWriteFactory();
}

ID3D11Device* getD3DDevice()
{
    return DCompCompositor::instance().getD3DDevice();
}

IDXGIDevice* getDXGIDevice()
{
    return DCompCompositor::instance().getDXGIDevice();
}

ID2D1Device* getD2DDevice()
{
    return DCompCompositor::instance().getD2DDevice();
}

IDCompositionDesktopDevice* getCompositionDevice()
{
    return DCompCompositor::instance().getDevice();
}

bool isCompositorInitialized()
{
    return DCompCompositor::instance().isInitialized();
}

void commitComposition()
{
    DCompCompositor::instance().commit();
}

uint64_t getCompositionGeneration()
{
    return DCompCompositor::instance().getGeneration();
}

bool handleDeviceLossIfNeeded(HRESULT hr)
{
    return DCompCompositor::instance().recoverFromDeviceLoss(hr);
}

} // namespace eacp::Graphics

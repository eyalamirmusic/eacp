#include <cstdio>

#include "../DComp-Windows.h"

#include "../Common.h"

#include <d3d11.h>
#include <dxgi1_2.h>

namespace eacp::Graphics
{

// Defined in CompositionHostWindow-Windows.cpp.
void rebuildAllCompositionHosts();

namespace
{
Vector<std::function<void()>>& renderingDeviceReplacedListeners()
{
    static auto listeners = Vector<std::function<void()>> {};
    return listeners;
}

// Listeners must tolerate being called with an unchanged device.
void handleRenderingDeviceReplaced()
{
    rebuildAllCompositionHosts();

    for (auto& listener: renderingDeviceReplacedListeners())
        listener();
}
} // namespace

// Fires after the shared D3D/D2D device was replaced. Listeners are never
// unregistered, so register only from process-lifetime objects. Main thread.
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

    // A failed Commit is itself a device-loss signal.
    void commit()
    {
        if (!device)
            return;

        if (auto hr = device->Commit(); FAILED(hr))
            recoverFromDeviceLoss(hr);
    }

private:
    // Device creation fails (usually E_ACCESSDENIED) with no accessible desktop
    // compositor. Must not throw into a host that installed no handler: every
    // consumer guards a null device and degrades to no GPU compositing.
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

        // No SINGLETHREADED: the composition engine may touch this off-thread.
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

        // Everything built against the old device is dead; moving the
        // generation tells every holder to rebuild.
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

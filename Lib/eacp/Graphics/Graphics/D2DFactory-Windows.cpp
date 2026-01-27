#define NOMINMAX
#include <Windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <d2d1_1.h>
#include <dwrite.h>
#include <wrl/client.h>
#include <shellscalingapi.h>

#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.UI.Composition.h>
#include <winrt/Windows.UI.Composition.Desktop.h>
#include <windows.ui.composition.interop.h>

#pragma comment(lib, "d2d1.lib")
#pragma comment(lib, "dwrite.lib")
#pragma comment(lib, "shcore.lib")
#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")

namespace wuc = winrt::Windows::UI::Composition;

namespace eacp::Graphics
{

using Microsoft::WRL::ComPtr;

class WinRTCompositor
{
public:
    static WinRTCompositor& instance()
    {
        static WinRTCompositor instance;
        return instance;
    }

    ID2D1Factory1* getD2DFactory() { return d2dFactory.Get(); }
    IDWriteFactory* getDWriteFactory() { return dwriteFactory.Get(); }
    ID3D11Device* getD3DDevice() { return d3dDevice.Get(); }
    IDXGIDevice* getDXGIDevice() { return dxgiDevice.Get(); }
    ID2D1Device* getD2DDevice() { return d2dDevice.Get(); }
    wuc::Compositor getCompositor() { return compositor; }
    wuc::CompositionGraphicsDevice getGraphicsDevice() { return graphicsDevice; }

    bool isInitialized() const { return initialized; }

private:
    WinRTCompositor()
    {
        // Enable per-monitor DPI awareness for crisp rendering on high-DPI displays
        SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

        // Initialize WinRT apartment
        winrt::init_apartment(winrt::apartment_type::single_threaded);

        // Create DirectWrite factory first (independent of D3D chain)
        DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                            reinterpret_cast<IUnknown**>(dwriteFactory.GetAddressOf()));

        // Create D3D11 device with BGRA support for D2D interop
        D3D_FEATURE_LEVEL featureLevels[] = {D3D_FEATURE_LEVEL_11_1,
                                              D3D_FEATURE_LEVEL_11_0,
                                              D3D_FEATURE_LEVEL_10_1,
                                              D3D_FEATURE_LEVEL_10_0};
        D3D_FEATURE_LEVEL featureLevel;

        HRESULT hr = D3D11CreateDevice(
            nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
            D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_SINGLETHREADED,
            featureLevels, ARRAYSIZE(featureLevels), D3D11_SDK_VERSION,
            d3dDevice.GetAddressOf(), &featureLevel, nullptr);

        if (FAILED(hr))
        {
            // Fallback to WARP software renderer
            hr = D3D11CreateDevice(
                nullptr, D3D_DRIVER_TYPE_WARP, nullptr,
                D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_SINGLETHREADED,
                featureLevels, ARRAYSIZE(featureLevels), D3D11_SDK_VERSION,
                d3dDevice.GetAddressOf(), &featureLevel, nullptr);
        }

        if (FAILED(hr))
            return;

        // Get DXGI device from D3D device
        hr = d3dDevice.As(&dxgiDevice);
        if (FAILED(hr))
            return;

        // Create D2D factory (need ID2D1Factory1 for device creation)
        hr = D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED,
                                d2dFactory.GetAddressOf());
        if (FAILED(hr))
            return;

        // Create D2D device from DXGI device
        hr = d2dFactory->CreateDevice(dxgiDevice.Get(), d2dDevice.GetAddressOf());
        if (FAILED(hr))
            return;

        // Create WinRT Compositor
        compositor = wuc::Compositor();

        // Create CompositionGraphicsDevice via interop
        auto interop = compositor.as<ABI::Windows::UI::Composition::ICompositorInterop>();
        winrt::com_ptr<ABI::Windows::UI::Composition::ICompositionGraphicsDevice> abiDevice;
        hr = interop->CreateGraphicsDevice(d2dDevice.Get(), abiDevice.put());
        if (FAILED(hr))
            return;

        graphicsDevice = abiDevice.as<wuc::CompositionGraphicsDevice>();

        initialized = true;
    }

    ~WinRTCompositor()
    {
        graphicsDevice = nullptr;
        compositor = nullptr;
        d2dDevice.Reset();
        d2dFactory.Reset();
        dxgiDevice.Reset();
        d3dDevice.Reset();
        dwriteFactory.Reset();
        winrt::uninit_apartment();
    }

    bool initialized = false;
    ComPtr<ID3D11Device> d3dDevice;
    ComPtr<IDXGIDevice> dxgiDevice;
    ComPtr<ID2D1Factory1> d2dFactory;
    ComPtr<ID2D1Device> d2dDevice;
    ComPtr<IDWriteFactory> dwriteFactory;
    wuc::Compositor compositor{nullptr};
    wuc::CompositionGraphicsDevice graphicsDevice{nullptr};
};

// Helper functions for easy access
inline ID2D1Factory1* getD2DFactory()
{
    return WinRTCompositor::instance().getD2DFactory();
}

inline IDWriteFactory* getDWriteFactory()
{
    return WinRTCompositor::instance().getDWriteFactory();
}

inline ID3D11Device* getD3DDevice()
{
    return WinRTCompositor::instance().getD3DDevice();
}

inline IDXGIDevice* getDXGIDevice()
{
    return WinRTCompositor::instance().getDXGIDevice();
}

inline ID2D1Device* getD2DDevice()
{
    return WinRTCompositor::instance().getD2DDevice();
}

inline wuc::Compositor getWinRTCompositor()
{
    return WinRTCompositor::instance().getCompositor();
}

inline wuc::CompositionGraphicsDevice getCompositionGraphicsDevice()
{
    return WinRTCompositor::instance().getGraphicsDevice();
}

inline bool isCompositorInitialized()
{
    return WinRTCompositor::instance().isInitialized();
}

} // namespace eacp::Graphics

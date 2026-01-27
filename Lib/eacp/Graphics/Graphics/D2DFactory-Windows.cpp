// Direct2D, DirectWrite, and DirectComposition factory initialization for Windows
// Provides global access to D2D1Factory, DWriteFactory, and DirectComposition devices

#define NOMINMAX
#include <Windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <d2d1_1.h>
#include <dwrite.h>
#include <dcomp.h>
#include <wrl/client.h>
#include <shellscalingapi.h>

#pragma comment(lib, "d2d1.lib")
#pragma comment(lib, "dwrite.lib")
#pragma comment(lib, "shcore.lib")
#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "dcomp.lib")

namespace eacp::Graphics
{

using Microsoft::WRL::ComPtr;

class DCompFactory
{
public:
    static DCompFactory& instance()
    {
        static DCompFactory instance;
        return instance;
    }

    ID2D1Factory1* getD2DFactory() { return d2dFactory.Get(); }
    IDWriteFactory* getDWriteFactory() { return dwriteFactory.Get(); }
    ID3D11Device* getD3DDevice() { return d3dDevice.Get(); }
    IDXGIDevice* getDXGIDevice() { return dxgiDevice.Get(); }
    ID2D1Device* getD2DDevice() { return d2dDevice.Get(); }
    IDCompositionDesktopDevice* getDCompDevice() { return dcompDevice.Get(); }

    bool isInitialized() const { return initialized; }

private:
    DCompFactory()
    {
        // Enable per-monitor DPI awareness for crisp rendering on high-DPI displays
        SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

        // Initialize COM
        CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

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

        // Create DirectComposition device from D2D device
        hr = DCompositionCreateDevice2(d2dDevice.Get(),
                                        IID_PPV_ARGS(dcompDevice.GetAddressOf()));
        if (FAILED(hr))
            return;

        initialized = true;
    }

    ~DCompFactory()
    {
        dcompDevice.Reset();
        d2dDevice.Reset();
        d2dFactory.Reset();
        dxgiDevice.Reset();
        d3dDevice.Reset();
        dwriteFactory.Reset();
        CoUninitialize();
    }

    bool initialized = false;
    ComPtr<ID3D11Device> d3dDevice;
    ComPtr<IDXGIDevice> dxgiDevice;
    ComPtr<ID2D1Factory1> d2dFactory;
    ComPtr<ID2D1Device> d2dDevice;
    ComPtr<IDCompositionDesktopDevice> dcompDevice;
    ComPtr<IDWriteFactory> dwriteFactory;
};

// Helper functions for easy access
inline ID2D1Factory1* getD2DFactory()
{
    return DCompFactory::instance().getD2DFactory();
}

inline IDWriteFactory* getDWriteFactory()
{
    return DCompFactory::instance().getDWriteFactory();
}

inline ID3D11Device* getD3DDevice()
{
    return DCompFactory::instance().getD3DDevice();
}

inline IDXGIDevice* getDXGIDevice()
{
    return DCompFactory::instance().getDXGIDevice();
}

inline ID2D1Device* getD2DDevice()
{
    return DCompFactory::instance().getD2DDevice();
}

inline IDCompositionDesktopDevice* getDCompDevice()
{
    return DCompFactory::instance().getDCompDevice();
}

inline bool isDCompInitialized()
{
    return DCompFactory::instance().isInitialized();
}

} // namespace eacp::Graphics

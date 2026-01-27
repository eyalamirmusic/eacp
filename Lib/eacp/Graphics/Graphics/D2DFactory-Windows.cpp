#include <d3d11.h>
#include <dxgi1_2.h>
#include <d2d1_1.h>
#include <dwrite.h>

#include <winrt/Windows.UI.Composition.h>
#include <windows.ui.composition.interop.h>

#include <array>

namespace wuc = winrt::Windows::UI::Composition;

namespace eacp::Graphics
{

class WinRTCompositor
{
public:
    static WinRTCompositor& instance()
    {
        static auto instance = WinRTCompositor();
        return instance;
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
    WinRTCompositor()
    {
        winrt::init_apartment(winrt::apartment_type::single_threaded);

        // Create DirectWrite factory
        winrt::check_hresult(
            DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED,
                                __uuidof(IDWriteFactory),
                                reinterpret_cast<IUnknown**>(dwriteFactory.put())));

        // Create D3D11 device with BGRA support for D2D interop
        std::array featureLevels = {D3D_FEATURE_LEVEL_11_1,
                                    D3D_FEATURE_LEVEL_11_0,
                                    D3D_FEATURE_LEVEL_10_1,
                                    D3D_FEATURE_LEVEL_10_0};
        D3D_FEATURE_LEVEL featureLevel;

        auto hr = D3D11CreateDevice(nullptr,
                                    D3D_DRIVER_TYPE_HARDWARE,
                                    nullptr,
                                    D3D11_CREATE_DEVICE_BGRA_SUPPORT
                                        | D3D11_CREATE_DEVICE_SINGLETHREADED,
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
                                  D3D11_CREATE_DEVICE_BGRA_SUPPORT
                                      | D3D11_CREATE_DEVICE_SINGLETHREADED,
                                  featureLevels.data(),
                                  static_cast<UINT>(featureLevels.size()),
                                  D3D11_SDK_VERSION,
                                  d3dDevice.put(),
                                  &featureLevel,
                                  nullptr));
        }

        // Get DXGI device from D3D device
        dxgiDevice = d3dDevice.as<IDXGIDevice>();

        // Create D2D factory
        winrt::check_hresult(
            D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, d2dFactory.put()));

        // Create D2D device from DXGI device
        winrt::check_hresult(
            d2dFactory->CreateDevice(dxgiDevice.get(), d2dDevice.put()));

        // Create WinRT Compositor
        compositor = wuc::Compositor();

        // Create CompositionGraphicsDevice via interop
        namespace Interop = ABI::Windows::UI::Composition;
        auto interop = compositor.as<Interop::ICompositorInterop>();
        winrt::com_ptr<Interop::ICompositionGraphicsDevice> abiDevice;
        winrt::check_hresult(
            interop->CreateGraphicsDevice(d2dDevice.get(), abiDevice.put()));

        graphicsDevice = abiDevice.as<wuc::CompositionGraphicsDevice>();

        initialized = true;
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
        winrt::uninit_apartment();
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

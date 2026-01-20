// Direct2D and DirectWrite factory initialization for Windows
// Provides global access to D2D1Factory and DWriteFactory

#define NOMINMAX
#include <Windows.h>
#include <d2d1.h>
#include <dwrite.h>
#include <wrl/client.h>

#pragma comment(lib, "d2d1.lib")
#pragma comment(lib, "dwrite.lib")

namespace eacp::Graphics
{

using Microsoft::WRL::ComPtr;

class D2DFactory
{
public:
    static D2DFactory& instance()
    {
        static D2DFactory instance;
        return instance;
    }

    ID2D1Factory* getD2DFactory() { return d2dFactory.Get(); }
    IDWriteFactory* getDWriteFactory() { return dwriteFactory.Get(); }

private:
    D2DFactory()
    {
        // Initialize COM
        CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

        // Create Direct2D factory
        D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, d2dFactory.GetAddressOf());

        // Create DirectWrite factory
        DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                            reinterpret_cast<IUnknown**>(dwriteFactory.GetAddressOf()));
    }

    ~D2DFactory()
    {
        dwriteFactory.Reset();
        d2dFactory.Reset();
        CoUninitialize();
    }

    ComPtr<ID2D1Factory> d2dFactory;
    ComPtr<IDWriteFactory> dwriteFactory;
};

// Helper functions for easy access
inline ID2D1Factory* getD2DFactory()
{
    return D2DFactory::instance().getD2DFactory();
}

inline IDWriteFactory* getDWriteFactory()
{
    return D2DFactory::instance().getDWriteFactory();
}

} // namespace eacp::Graphics

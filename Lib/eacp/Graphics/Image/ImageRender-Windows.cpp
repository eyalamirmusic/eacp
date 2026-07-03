#include <eacp/Core/Utils/WinInclude.h>

#include "ImageRender.h"
#include "ImageCodec.h"
#include "../Graphics/D2DContext-Windows.h"

#include <eacp/SIMD/SIMD.h>

namespace eacp::Graphics
{

ID2D1Device* getD2DDevice();
bool handleDeviceLossIfNeeded(HRESULT hr);

namespace
{
using Microsoft::WRL::ComPtr;

D2D1_BITMAP_PROPERTIES1 bitmapProperties(D2D1_BITMAP_OPTIONS options)
{
    return D2D1::BitmapProperties1(options,
                                   D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM,
                                                     D2D1_ALPHA_MODE_PREMULTIPLIED));
}

// Offscreen target for D2DContext: draws land in a GPU bitmap whose pixels
// are read back through a CPU staging copy after EndDraw.
struct OffscreenSurface : BackingSurface
{
    ComPtr<ID2D1DeviceContext> dc;
    ComPtr<ID2D1Bitmap1> target;
    bool drawFailed = false;

    bool create(int width, int height)
    {
        auto* device = getD2DDevice();
        if (!device)
            return false;

        if (FAILED(device->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE,
                                               dc.GetAddressOf())))
            return false;

        auto size =
            D2D1::SizeU(static_cast<UINT32>(width), static_cast<UINT32>(height));
        if (FAILED(dc->CreateBitmap(size,
                                    nullptr,
                                    0,
                                    bitmapProperties(D2D1_BITMAP_OPTIONS_TARGET),
                                    target.GetAddressOf())))
            return false;

        dc->SetTarget(target.Get());
        return true;
    }

    ID2D1DeviceContext* beginDraw(D2D1::Matrix3x2F& baseTransform) override
    {
        baseTransform = D2D1::Matrix3x2F::Identity();
        dc->BeginDraw();
        return dc.Get();
    }

    void endDraw() override
    {
        auto hr = dc->EndDraw();
        if (FAILED(hr))
        {
            handleDeviceLossIfNeeded(hr);
            drawFailed = true;
        }
    }

    bool hasSurface() const override { return target != nullptr; }

    // Target bitmaps can't be mapped; the pixels come back through a CPU-read
    // staging copy. Fills `rgba` with straight-alpha RGBA.
    bool readPixels(int width, int height, ImageData& rgba)
    {
        auto staging = ComPtr<ID2D1Bitmap1> {};
        auto size =
            D2D1::SizeU(static_cast<UINT32>(width), static_cast<UINT32>(height));
        if (FAILED(
                dc->CreateBitmap(size,
                                 nullptr,
                                 0,
                                 bitmapProperties(D2D1_BITMAP_OPTIONS_CPU_READ
                                                  | D2D1_BITMAP_OPTIONS_CANNOT_DRAW),
                                 staging.GetAddressOf())))
            return false;

        if (FAILED(staging->CopyFromBitmap(nullptr, target.Get(), nullptr)))
            return false;

        auto mapped = D2D1_MAPPED_RECT {};
        if (FAILED(staging->Map(D2D1_MAP_OPTIONS_READ, &mapped)))
            return false;

        rgba.resize(width * height * 4);
        auto rowBytes = static_cast<std::size_t>(width) * 4;
        for (auto y = 0; y < height; ++y)
        {
            const auto* row =
                mapped.bits + static_cast<std::size_t>(y) * mapped.pitch;
            simd::swapRedBlue(row,
                              rgba.data() + static_cast<std::size_t>(y) * rowBytes,
                              static_cast<std::size_t>(width));
        }

        staging->Unmap();
        detail::unpremultiply(rgba);
        return true;
    }
};
} // namespace

Image renderToImage(int width, int height, const std::function<void(Context&)>& draw)
{
    if (width <= 0 || height <= 0)
        return {};

    auto surface = OffscreenSurface {};
    if (!surface.create(width, height))
        return {};

    {
        auto context = D2DContext(surface);

        // Clear the target even when draw() never issues a call, so an
        // empty drawing still comes back fully transparent, not undefined.
        if (!context.ensureDrawing())
            return {};

        draw(context);
    }

    if (surface.drawFailed)
        return {};

    auto rgba = ImageData {};
    if (!surface.readPixels(width, height, rgba))
        return {};

    return Image(width, height, std::move(rgba));
}

} // namespace eacp::Graphics

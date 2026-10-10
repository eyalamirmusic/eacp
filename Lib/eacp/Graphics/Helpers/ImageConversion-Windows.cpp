#include "ImageConversion-Windows.h"
#include "../Image/Image.h"

#include <ESIMD/ESIMD.h>

namespace eacp::Graphics
{

namespace
{
HICON createIconFromImage(const Image& image,
                          bool isIcon,
                          int hotspotX,
                          int hotspotY)
{
    auto width = image.width();
    auto height = image.height();

    if (width <= 0 || height <= 0)
        return nullptr;

    BITMAPV5HEADER header = {};
    header.bV5Size = sizeof(BITMAPV5HEADER);
    header.bV5Width = width;
    header.bV5Height = -height; // top-down
    header.bV5Planes = 1;
    header.bV5BitCount = 32;
    header.bV5Compression = BI_BITFIELDS;
    header.bV5RedMask = 0x00FF0000;
    header.bV5GreenMask = 0x0000FF00;
    header.bV5BlueMask = 0x000000FF;
    header.bV5AlphaMask = 0xFF000000;

    void* bits = nullptr;
    auto hdc = GetDC(nullptr);
    auto colorBitmap = CreateDIBSection(hdc,
                                        reinterpret_cast<BITMAPINFO*>(&header),
                                        DIB_RGB_COLORS,
                                        &bits,
                                        nullptr,
                                        0);
    ReleaseDC(nullptr, hdc);

    if (!colorBitmap || !bits)
    {
        if (colorBitmap)
            DeleteObject(colorBitmap);
        return nullptr;
    }

    // RGBA (source) -> BGRA (DIB byte order).
    auto* dst = static_cast<std::uint8_t*>(bits);
    const auto* src = image.pixels().data();
    esimd::swapRedBlue(src, dst, width * height);

    auto maskBitmap = CreateBitmap(width, height, 1, 1, nullptr);

    ICONINFO iconInfo = {};
    iconInfo.fIcon = isIcon ? TRUE : FALSE;
    iconInfo.xHotspot = static_cast<DWORD>(hotspotX);
    iconInfo.yHotspot = static_cast<DWORD>(hotspotY);
    iconInfo.hbmColor = colorBitmap;
    iconInfo.hbmMask = maskBitmap;

    auto icon = CreateIconIndirect(&iconInfo);

    DeleteObject(colorBitmap);
    DeleteObject(maskBitmap);

    return icon;
}
} // namespace

HICON toHIcon(const Image& image)
{
    return createIconFromImage(image, true, 0, 0);
}

HCURSOR toHCursor(const Image& image, int hotspotX, int hotspotY)
{
    return static_cast<HCURSOR>(
        createIconFromImage(image, false, hotspotX, hotspotY));
}

} // namespace eacp::Graphics

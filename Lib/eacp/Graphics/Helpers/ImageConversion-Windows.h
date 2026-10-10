#pragma once

#include <eacp/Core/Utils/WinInclude.h>

namespace eacp::Graphics
{
class Image;

// Builds a 32bpp ARGB HICON from the Image's straight RGBA pixels. Returns
// nullptr on an empty image or GDI failure. The caller owns the icon and
// releases it with DestroyIcon.
HICON toHIcon(const Image& image);

// The same pixels as a cursor whose hotspot is (hotspotX, hotspotY) in the
// image's pixels. Released with DestroyIcon, as toHIcon's result is.
HCURSOR toHCursor(const Image& image, int hotspotX, int hotspotY);
} // namespace eacp::Graphics

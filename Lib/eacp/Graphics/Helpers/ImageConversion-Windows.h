#pragma once

#include <eacp/Core/Utils/WinInclude.h>
#include <eacp/Graphics/Image/Image.h>

namespace eacp::Graphics
{
// Builds a 32bpp ARGB HICON from the Image's straight RGBA pixels. Returns
// nullptr on an empty image or GDI failure. The caller owns the icon and
// releases it with DestroyIcon.
HICON toHIcon(const Image& image);
} // namespace eacp::Graphics

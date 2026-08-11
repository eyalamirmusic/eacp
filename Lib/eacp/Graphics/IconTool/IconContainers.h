#pragma once

#include "../Common.h"

#include "../Image/Image.h"

// Assembles platform icon containers from a single source image, wrapping PNG
// frames encoded by Image::toPng.
namespace eacp::Graphics::Icons
{

// Halves repeatedly before the final bilinear step, approximating an area
// filter to avoid the aliasing of one large-factor reduction.
Image downscaleTo(const Image& src, int size);

// 'icns' magic + total length, then one PNG-payload chunk per standard size.
void writeIcns(const Image& src, const FilePath& out);

// ICONDIR + one ICONDIRENTRY and PNG payload per standard size.
void writeIco(const Image& src, const FilePath& out);

// An .appiconset directory holding the marketing PNG and a Contents.json.
void writeIconset(const Image& src, const FilePath& outDir);

} // namespace eacp::Graphics::Icons

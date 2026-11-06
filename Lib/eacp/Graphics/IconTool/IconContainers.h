#pragma once

/** @file Writes .icns, .ico and iOS .appiconset icon files from one source image. */

#include "../Common.h"

#include "../Image/Image.h"

namespace eacp::Graphics::Icons
{

/// Produce a size*size RGBA copy of src by halving repeatedly before the final
/// bilinear step, which approximates an area filter and avoids the aliasing a
/// single large-factor reduction would introduce.
Image downscaleTo(const Image& src, int size);

/// Apple .icns: 'icns' magic + total length, then one PNG-payload chunk per
/// standard size. Written for the macOS/iOS bundle icon.
void writeIcns(const Image& src, const FilePath& out);

/// Windows .ico: ICONDIR + one ICONDIRENTRY and PNG payload per standard size,
/// compiled into the executable as an ICON resource.
void writeIco(const Image& src, const FilePath& out);

/// iOS asset catalog: an .appiconset directory holding the marketing PNG and a
/// Contents.json. Emitted for completeness; the CMake wiring is a follow-up.
void writeIconset(const Image& src, const FilePath& outDir);

} // namespace eacp::Graphics::Icons

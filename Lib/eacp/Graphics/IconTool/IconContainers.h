#pragma once

#include <eacp/Graphics/Image/Image.h>
#include <filesystem>

// Assembles platform icon containers from a single source image, wrapping PNG
// frames the framework already knows how to encode (Image::toPng). The byte
// layouts are the only platform-specific part; decoding, resizing and PNG
// encoding all reuse eacp-graphics.
namespace eacp::Graphics::Icons
{

// Produce a size*size RGBA copy of src by halving repeatedly before the final
// bilinear step, which approximates an area filter and avoids the aliasing a
// single large-factor reduction would introduce.
Image downscaleTo(const Image& src, int size);

// Apple .icns: 'icns' magic + total length, then one PNG-payload chunk per
// standard size. Written for the macOS/iOS bundle icon.
void writeIcns(const Image& src, const std::filesystem::path& out);

// Windows .ico: ICONDIR + one ICONDIRENTRY and PNG payload per standard size,
// compiled into the executable as an ICON resource.
void writeIco(const Image& src, const std::filesystem::path& out);

// iOS asset catalog: an .appiconset directory holding the marketing PNG and a
// Contents.json. Emitted for completeness; the CMake wiring is a follow-up.
void writeIconset(const Image& src, const std::filesystem::path& outDir);

} // namespace eacp::Graphics::Icons

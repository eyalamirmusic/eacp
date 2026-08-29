#pragma once

// Direct2D / DirectWrite only, for the drawing primitives (Path, Font,
// TextMetrics, Image) that never touch the compositor. Anything that builds a
// visual tree wants DComp-Windows.h, which includes this.

#include <eacp/Core/Utils/WinInclude.h>

#include <d2d1_1.h>
#include <dwrite.h>
#include <wrl/client.h>

#include <optional>
#include <string>

namespace eacp::Graphics
{
// The shared font collection (FontRegistry-Windows.cpp) — the Windows stand-in
// for CoreText's process-wide registry: the system fonts plus every font
// registered from memory. Font, TextMetrics and the eacp-text rasterizer all
// resolve against this one collection so an embedded face is visible
// everywhere at once.
Microsoft::WRL::ComPtr<IDWriteFontCollection> getFontCollection();

// What registering a font made visible: the family the collection files the
// face under, and the face's PostScript name.
struct RegisteredFontNames
{
    std::wstring family;
    std::wstring postScriptName;
};

// Registers in-memory OTF/TTF bytes with the shared collection (the bytes are
// copied). The Windows half of eacp::Text::registerMemoryFont. Nothing when
// the bytes are not a usable font.
std::optional<RegisteredFontNames> registerMemoryFontData(const void* data,
                                                          std::size_t size);

// Resolves `name` the way CTFontCreateWithName does — as a family name, then a
// PostScript or full face name — so the one name a caller ships works on both
// platforms. Empty when nothing in the collection matches.
std::wstring resolveFontFamilyName(const std::wstring& name);
} // namespace eacp::Graphics

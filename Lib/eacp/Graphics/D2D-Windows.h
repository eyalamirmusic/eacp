#pragma once

// Direct2D / DirectWrite only, for the drawing primitives that never touch the
// compositor. A visual tree wants DComp-Windows.h, which includes this.

#include <eacp/Core/Utils/WinInclude.h>

#include <d2d1_1.h>
#include <dwrite.h>
#include <wrl/client.h>

#include <string>

namespace eacp::Graphics
{
// The system fonts plus every font registered from memory; everything resolves
// against this one collection. See FontRegistry-Windows.cpp.
Microsoft::WRL::ComPtr<IDWriteFontCollection> getFontCollection();

// Copies the bytes. The Windows half of eacp::Text::registerMemoryFont.
bool registerMemoryFontData(const void* data, std::size_t size);

// Resolves `name` the way CTFontCreateWithName does: family name first, then
// PostScript or full face name. Empty when nothing matches.
std::wstring resolveFontFamilyName(const std::wstring& name);
} // namespace eacp::Graphics

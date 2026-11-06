#pragma once

/** @file A platform font by name and size. EACP_HAS_CONTEXT only (Apple and
    Windows). */

#include "../Common.h"

#include "Primitives.h"

namespace eacp::Graphics
{

// A font name (family or PostScript name) and a size in points.
struct FontOptions
{
    FontOptions withName(const std::string& newName) const;
    FontOptions withSize(float newSize);

    std::string name = "Helvetica";
    float size = 12.f;
};

// A Core Text font on Apple, a DirectWrite text format on Windows. For text on
// Linux, Android or the GPU, use the Text module instead.
class Font
{
public:
    Font(const FontOptions& optionsToUse = {});
    ~Font() = default;

    void setFont(const FontOptions& optionsToUse);
    // CTFontRef on Apple, IDWriteTextFormat* on Windows. Owned by the Font.
    void* getHandle() const;

private:
    void updateNativeFont();

    FontOptions options;

    struct Native;
    Pimpl<Native> impl;
};

} // namespace eacp::Graphics

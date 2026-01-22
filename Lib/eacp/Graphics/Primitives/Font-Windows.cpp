// Windows implementation of Font using DirectWrite
#include "Font.h"

#include <cassert>

#define NOMINMAX
#include <Windows.h>
#include <dwrite.h>
#include <wrl/client.h>

// Forward declaration of factory access
namespace eacp::Graphics
{
IDWriteFactory* getDWriteFactory();
}

namespace eacp::Graphics
{

using Microsoft::WRL::ComPtr;

struct Font::Native
{
    Native() {}

    void update(const FontOptions& options)
    {
        textFormat.Reset();

        auto* factory = getDWriteFactory();
        if (!factory)
            return;

        // Convert font name to wide string
        int len =
            MultiByteToWideChar(CP_UTF8, 0, options.name.c_str(), -1, nullptr, 0);
        std::wstring wideName(len, L'\0');
        MultiByteToWideChar(CP_UTF8, 0, options.name.c_str(), -1, wideName.data(), len);

        // Create text format
        factory->CreateTextFormat(wideName.c_str(), nullptr, DWRITE_FONT_WEIGHT_NORMAL,
                                  DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                                  options.size, L"en-us", textFormat.GetAddressOf());

        // If font not found, try with Arial as fallback
        if (!textFormat)
        {
            factory->CreateTextFormat(L"Arial", nullptr, DWRITE_FONT_WEIGHT_NORMAL,
                                      DWRITE_FONT_STYLE_NORMAL,
                                      DWRITE_FONT_STRETCH_NORMAL, options.size, L"en-us",
                                      textFormat.GetAddressOf());
        }

        if (textFormat)
        {
            textFormat->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        }
    }

    ComPtr<IDWriteTextFormat> textFormat;
};

Font::Font(const FontOptions& optionsToUse)
    : options(optionsToUse)
    , impl()
{
    updateNativeFont();
}

void Font::setFont(const FontOptions& optionsToUse)
{
    options = optionsToUse;
    updateNativeFont();
}

void* Font::getHandle() const
{
    return impl->textFormat.Get();
}

void Font::updateNativeFont()
{
    impl->update(options);
}

} // namespace eacp::Graphics

#include "Font.h"
#include "../Helpers/StringUtils-Windows.h"


#include <dwrite.h>
#include <wrl/client.h>

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

        auto wideName = toWideString(options.name);

        factory->CreateTextFormat(wideName.c_str(), nullptr, DWRITE_FONT_WEIGHT_NORMAL,
                                  DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                                  options.size, L"en-us", textFormat.GetAddressOf());

        if (!textFormat)
        {
            factory->CreateTextFormat(L"Arial", nullptr, DWRITE_FONT_WEIGHT_NORMAL,
                                      DWRITE_FONT_STYLE_NORMAL,
                                      DWRITE_FONT_STRETCH_NORMAL, options.size, L"en-us",
                                      textFormat.GetAddressOf());
        }

        if (textFormat)
            textFormat->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
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

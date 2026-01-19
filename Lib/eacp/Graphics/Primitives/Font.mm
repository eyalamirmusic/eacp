#include "Font.h"
#include "GraphicUtils.h"
#import <CoreText/CoreText.h>

namespace eacp::Graphics
{

struct Font::Native
{
    void reset(const FontOptions& options)
    {
        CFRef<CFStringRef> name(CFStringCreateWithCString(
            nullptr, options.name.c_str(), kCFStringEncodingUTF8));
        font.reset(CTFontCreateWithName(name, options.size, nullptr));
    }

    void setSize(float size)
    {
        if (font)
            font.reset(CTFontCreateCopyWithAttributes(font, size, nullptr, nullptr));
    }

    float getSize() const
    {
        if (font)
            return (float) CTFontGetSize(font);

        return 12.0f;
    }

    CFRef<CTFontRef> font;
};

Font::Font(const FontOptions& optionsToUse)
{
    setFont(optionsToUse);
}

void* Font::getHandle() const
{
    return (void*) impl->font.get();
}

void Font::updateNativeFont()
{
    impl->reset(options);
}

void Font::setFont(const FontOptions& optionsToUse)
{
    options = optionsToUse;
    updateNativeFont();
}

} // namespace eacp::Graphics

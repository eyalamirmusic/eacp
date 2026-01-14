#include "Font.h"
#include "MacGraphicUtils.h"
#import <CoreText/CoreText.h>

namespace eacp::Graphics
{

struct Font::Native
{
    Native() { reset("Helvetica", 12.0); }
    Native(const std::string& fontName, float size) { reset(fontName, size); }

    void reset(const std::string& fontName, float size)
    {
        CFRef<CFStringRef> name(CFStringCreateWithCString(
            nullptr, fontName.c_str(), kCFStringEncodingUTF8));
        font.reset(CTFontCreateWithName(name, size, nullptr));
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

Font::Font()
    : impl()
{
}

Font::Font(const std::string& fontName, float size)
    : impl(fontName, size)
{
}

void Font::setSize(float size)
{
    impl->setSize(size);
}

float Font::getSize() const
{
    return impl->getSize();
}

void* Font::getHandle() const
{
    return (void*) impl->font.get();
}

} // namespace eacp::Graphics

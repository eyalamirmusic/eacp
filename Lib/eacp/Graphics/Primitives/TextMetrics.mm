#include "TextMetrics.h"
#include "MacGraphicUtils.h"
#import <CoreText/CoreText.h>
#import <Foundation/Foundation.h>
#include <eacp/Core/ObjC/Strings.h>

namespace eacp::Graphics
{

float TextMetrics::measureWidth(const std::string& text, const Font& font)
{
    if (text.empty())
        return 0.f;

    CTFontRef ctFont = (CTFontRef) font.getHandle();

    CFRef<CFStringRef> cfString(CFStringCreateWithCString(
        nullptr, text.c_str(), kCFStringEncodingUTF8));

    CFStringRef keys[] = {kCTFontAttributeName};
    CFTypeRef values[] = {ctFont};

    CFRef<CFDictionaryRef> attributes(CFDictionaryCreate(
        nullptr, (const void**) keys, (const void**) values, 1,
        &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks));

    CFRef<CFAttributedStringRef> attrString(
        CFAttributedStringCreate(nullptr, cfString, attributes));

    CFRef<CTLineRef> line(CTLineCreateWithAttributedString(attrString));

    return (float) CTLineGetTypographicBounds(line, nullptr, nullptr, nullptr);
}

float TextMetrics::getOffsetForIndex(const std::string& text,
                                     size_t index,
                                     const Font& font)
{
    if (text.empty() || index == 0)
        return 0.f;

    CTFontRef ctFont = (CTFontRef) font.getHandle();

    CFRef<CFStringRef> cfString(CFStringCreateWithCString(
        nullptr, text.c_str(), kCFStringEncodingUTF8));

    CFStringRef keys[] = {kCTFontAttributeName};
    CFTypeRef values[] = {ctFont};

    CFRef<CFDictionaryRef> attributes(CFDictionaryCreate(
        nullptr, (const void**) keys, (const void**) values, 1,
        &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks));

    CFRef<CFAttributedStringRef> attrString(
        CFAttributedStringCreate(nullptr, cfString, attributes));

    CFRef<CTLineRef> line(CTLineCreateWithAttributedString(attrString));

    CGFloat offset = CTLineGetOffsetForStringIndex(line, (CFIndex) index, nullptr);
    return (float) offset;
}

size_t TextMetrics::getIndexForOffset(const std::string& text,
                                      float xOffset,
                                      const Font& font)
{
    if (text.empty())
        return 0;

    CTFontRef ctFont = (CTFontRef) font.getHandle();

    CFRef<CFStringRef> cfString(CFStringCreateWithCString(
        nullptr, text.c_str(), kCFStringEncodingUTF8));

    CFStringRef keys[] = {kCTFontAttributeName};
    CFTypeRef values[] = {ctFont};

    CFRef<CFDictionaryRef> attributes(CFDictionaryCreate(
        nullptr, (const void**) keys, (const void**) values, 1,
        &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks));

    CFRef<CFAttributedStringRef> attrString(
        CFAttributedStringCreate(nullptr, cfString, attributes));

    CFRef<CTLineRef> line(CTLineCreateWithAttributedString(attrString));

    CFIndex index =
        CTLineGetStringIndexForPosition(line, CGPointMake(xOffset, 0.f));

    if (index == kCFNotFound)
        return text.length();

    return (size_t) index;
}

float TextMetrics::getLineHeight(const Font& font)
{
    CTFontRef ctFont = (CTFontRef) font.getHandle();

    CGFloat ascent = CTFontGetAscent(ctFont);
    CGFloat descent = CTFontGetDescent(ctFont);
    CGFloat leading = CTFontGetLeading(ctFont);

    return (float) (ascent + descent + leading);
}

} // namespace eacp::Graphics

#include "TextMetrics.h"
#include "GraphicUtils.h"
#import <CoreText/CoreText.h>
#import <Foundation/Foundation.h>
#include <eacp/Core/ObjC/Strings.h>

namespace eacp::Graphics
{

float TextMetrics::measureWidth(const std::string& text, const Font& font)
{
    if (text.empty())
        return 0.f;

    auto ctFont = (CTFontRef) font.getHandle();

    auto cfString = CFRef<CFStringRef>(CFStringCreateWithCString(
        nullptr, text.c_str(), kCFStringEncodingUTF8));

    CFStringRef keys[] = {kCTFontAttributeName};
    CFTypeRef values[] = {ctFont};

    auto attributes = CFRef<CFDictionaryRef>(CFDictionaryCreate(
        nullptr, (const void**) keys, (const void**) values, 1,
        &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks));

    auto attrString = CFRef<CFAttributedStringRef>(
        CFAttributedStringCreate(nullptr, cfString, attributes));

    auto line = CFRef<CTLineRef>(CTLineCreateWithAttributedString(attrString));

    return (float) CTLineGetTypographicBounds(line, nullptr, nullptr, nullptr);
}

float TextMetrics::getOffsetForIndex(const std::string& text,
                                     int index,
                                     const Font& font)
{
    if (text.empty() || index <= 0)
        return 0.f;

    auto ctFont = (CTFontRef) font.getHandle();

    auto cfString = CFRef<CFStringRef>(CFStringCreateWithCString(
        nullptr, text.c_str(), kCFStringEncodingUTF8));

    CFStringRef keys[] = {kCTFontAttributeName};
    CFTypeRef values[] = {ctFont};

    auto attributes = CFRef<CFDictionaryRef>(CFDictionaryCreate(
        nullptr, (const void**) keys, (const void**) values, 1,
        &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks));

    auto attrString = CFRef<CFAttributedStringRef>(
        CFAttributedStringCreate(nullptr, cfString, attributes));

    auto line = CFRef<CTLineRef>(CTLineCreateWithAttributedString(attrString));

    auto offset = CTLineGetOffsetForStringIndex(line, (CFIndex) index, nullptr);
    return (float) offset;
}

int TextMetrics::getIndexForOffset(const std::string& text,
                                   float xOffset,
                                   const Font& font)
{
    if (text.empty())
        return 0;

    auto ctFont = (CTFontRef) font.getHandle();

    auto cfString = CFRef<CFStringRef>(CFStringCreateWithCString(
        nullptr, text.c_str(), kCFStringEncodingUTF8));

    CFStringRef keys[] = {kCTFontAttributeName};
    CFTypeRef values[] = {ctFont};

    auto attributes = CFRef<CFDictionaryRef>(CFDictionaryCreate(
        nullptr, (const void**) keys, (const void**) values, 1,
        &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks));

    auto attrString = CFRef<CFAttributedStringRef>(
        CFAttributedStringCreate(nullptr, cfString, attributes));

    auto line = CFRef<CTLineRef>(CTLineCreateWithAttributedString(attrString));

    auto index =
        CTLineGetStringIndexForPosition(line, CGPointMake(xOffset, 0.f));

    if (index == kCFNotFound)
        return (int) text.length();

    return (int) index;
}

float TextMetrics::getLineHeight(const Font& font)
{
    auto ctFont = (CTFontRef) font.getHandle();

    auto ascent = CTFontGetAscent(ctFont);
    auto descent = CTFontGetDescent(ctFont);
    auto leading = CTFontGetLeading(ctFont);

    return (float) (ascent + descent + leading);
}

float TextMetrics::getAscent(const Font& font)
{
    auto ctFont = (CTFontRef) font.getHandle();
    return (float) CTFontGetAscent(ctFont);
}

float TextMetrics::getDescent(const Font& font)
{
    auto ctFont = (CTFontRef) font.getHandle();
    return (float) CTFontGetDescent(ctFont);
}

} // namespace eacp::Graphics

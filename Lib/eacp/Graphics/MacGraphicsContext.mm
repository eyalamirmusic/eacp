#include "MacGraphicsContext.h"
#import <CoreText/CoreText.h>
#include "../ObjC/CFRef.h"

namespace eacp::Graphics
{

static CFRef<CGColorSpaceRef> getColorSpace()
{
    return {CGColorSpaceCreateDeviceRGB()};
}

static CFRef<CGColorRef> getColorRef(const Color& c)
{
    auto colorSpace = getColorSpace();

    CGFloat components[4] = {c.r, c.g, c.b, c.a};
    return CGColorCreate(colorSpace, components);
}

void MacOSContext::drawText(const std::string& text,
                            const Point& position,
                            const Font& font)
{
    if (text.empty())
        return;

    auto ctFont = (CTFontRef) font.getHandle();

    if (!ctFont)
        return;

    CFRef<CFStringRef> cfString(
        CFStringCreateWithCString(nullptr, text.c_str(), kCFStringEncodingUTF8));

    CFRef<CFMutableAttributedStringRef> attrString(
        CFAttributedStringCreateMutable(nullptr, 0));

    CFAttributedStringReplaceString(attrString, CFRangeMake(0, 0), cfString);
    CFAttributedStringSetAttribute(attrString,
                                   CFRangeMake(0, CFStringGetLength(cfString)),
                                   kCTFontAttributeName,
                                   ctFont);

    auto textColor = getColorRef(currentColor);

    CFAttributedStringSetAttribute(attrString,
                                   CFRangeMake(0, CFStringGetLength(cfString)),
                                   kCTForegroundColorAttributeName,
                                   textColor);


    CFRef<CTLineRef> line(CTLineCreateWithAttributedString(attrString));

    CGContextSetTextMatrix(context, CGAffineTransformIdentity);

    CGContextTranslateCTM(context, position.x, position.y);
    CGContextScaleCTM(context, 1.0, -1.0);

    CTLineDraw(line, context);

    CGContextScaleCTM(context, 1.0, -1.0);
    CGContextTranslateCTM(context, -position.x, -position.y);
}

} // namespace eacp::Graphics

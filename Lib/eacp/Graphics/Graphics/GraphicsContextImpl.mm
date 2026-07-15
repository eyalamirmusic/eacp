#include "GraphicsContextImpl.h"
#import <CoreText/CoreText.h>
#import <QuartzCore/QuartzCore.h>

#include "../Image/Image.h"
#include "../Layers/NativeLayer.h"
#include "../View/View.h"

#include <cmath>

namespace eacp::Graphics
{

// Composites a view and its descendants into ctx the way the screen stacks
// them: the view's paint() backdrop first (a view-backed layer's delegate
// drawing is not reachable via renderInContext:, so we invoke it directly),
// then its attached shape/text layers, then child views -- each translated and
// clipped to its frame. Metal and web layers do not draw here.
static void compositeView(CGContextRef ctx, View& view)
{
    {
        auto painter = MacOSContext(ctx);
        view.paint(painter);
    }

    for (auto* layer: view.getLayers())
    {
        auto* native = (NativeLayer*) layer->getNativeLayer();
        if (native == nullptr)
            continue;

        auto* caLayer = native->nativeLayer;
        if (caLayer == nil || caLayer.isHidden)
            continue;

        auto frame = caLayer.frame;

        CGContextSaveGState(ctx);
        CGContextTranslateCTM(ctx, frame.origin.x, frame.origin.y);
        CGContextSetAlpha(ctx, caLayer.opacity);
        [caLayer renderInContext:ctx];
        CGContextRestoreGState(ctx);
    }

    for (auto* child: view.getSubviews())
    {
        auto bounds = child->getBounds();

        CGContextSaveGState(ctx);
        CGContextTranslateCTM(ctx, bounds.x, bounds.y);
        CGContextClipToRect(ctx, CGRectMake(0, 0, bounds.w, bounds.h));
        compositeView(ctx, *child);
        CGContextRestoreGState(ctx);
    }
}

Image renderLayerToImage(View& view, const Rect& bounds, float scale)
{
    auto pixelWidth = static_cast<int>(std::lround(bounds.w * scale));
    auto pixelHeight = static_cast<int>(std::lround(bounds.h * scale));

    if (pixelWidth <= 0 || pixelHeight <= 0)
        return {};

    auto colorSpace = CFRef<CGColorSpaceRef>(CGColorSpaceCreateDeviceRGB());
    auto bitmapInfo = static_cast<std::uint32_t>(kCGImageAlphaPremultipliedLast)
                      | static_cast<std::uint32_t>(kCGBitmapByteOrder32Big);

    auto context = CFRef<CGContextRef>(
        CGBitmapContextCreate(nullptr,
                              static_cast<std::size_t>(pixelWidth),
                              static_cast<std::size_t>(pixelHeight),
                              8,
                              0,
                              colorSpace,
                              bitmapInfo));
    if (!context)
        return {};

    // paint() and the shape/text layers draw top-left origin to match the
    // flipped on-screen view; a bitmap context is bottom-left, so flip it, then
    // scale points to pixels for the backing scale.
    CGContextTranslateCTM(context, 0, pixelHeight);
    CGContextScaleCTM(context, 1.0, -1.0);
    CGContextScaleCTM(context, scale, scale);

    compositeView(context.get(), view);

    auto image = CFRef<CGImageRef>(CGBitmapContextCreateImage(context));
    if (!image)
        return {};

    auto error = std::string {};
    return detail::imageFromCGImage(image.get(), error);
}

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

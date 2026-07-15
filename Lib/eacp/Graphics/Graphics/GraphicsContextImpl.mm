#include "GraphicsContextImpl.h"
#import <CoreText/CoreText.h>
#import <QuartzCore/QuartzCore.h>

#include "../Image/Image.h"
#include "../Layers/NativeLayer.h"
#include "../View/View.h"

#include <eacp/Core/Threads/Async.h>

#include <cmath>
#include <memory>

namespace eacp::Graphics
{

// Draws a top-left-origin straight-alpha Image into the y-down context, filling
// dest (in points). The bitmap context is flipped, so flip back locally -- as
// drawText does -- otherwise the image lands upside down.
void drawImageInContext(CGContextRef ctx, const Image& image, const Rect& dest)
{
    if (!image.isValid())
        return;

    auto byteCount = static_cast<std::size_t>(image.width()) * image.height() * 4;
    auto provider = CFRef<CGDataProviderRef>(CGDataProviderCreateWithData(
        nullptr, image.pixels().data(), byteCount, nullptr));

    auto colorSpace = CFRef<CGColorSpaceRef>(CGColorSpaceCreateDeviceRGB());
    auto bitmapInfo = static_cast<std::uint32_t>(kCGImageAlphaLast)
                      | static_cast<std::uint32_t>(kCGBitmapByteOrder32Big);

    auto cgImage = CFRef<CGImageRef>(
        CGImageCreate(static_cast<std::size_t>(image.width()),
                      static_cast<std::size_t>(image.height()),
                      8,
                      32,
                      static_cast<std::size_t>(image.width()) * 4,
                      colorSpace,
                      bitmapInfo,
                      provider,
                      nullptr,
                      false,
                      kCGRenderingIntentDefault));
    if (!cgImage)
        return;

    CGContextSaveGState(ctx);
    CGContextTranslateCTM(ctx, dest.x, dest.y + dest.h);
    CGContextScaleCTM(ctx, 1.0, -1.0);
    CGContextDrawImage(ctx, CGRectMake(0, 0, dest.w, dest.h), cgImage.get());
    CGContextRestoreGState(ctx);
}

// Composites a view and its descendants into ctx the way the screen stacks
// them: the view's paint() backdrop first (a view-backed layer's delegate
// drawing is not reachable via renderInContext:, so we invoke it directly),
// then its attached shape/text layers, then its own native content (a GPUView's
// Metal image), then child views -- each translated and clipped to its frame.
// Web content is drawn later, asynchronously, by the caller.
static void compositeView(CGContextRef ctx, View& view, float scale)
{
    // Group opacity flattens the whole subtree first, then fades it as one, so
    // overlapping children don't show through each other. A transparency layer
    // captures the current alpha and applies it on end; skip it at full opacity.
    auto grouped = view.getOpacity() < 1.0f;
    if (grouped)
    {
        CGContextSetAlpha(ctx, view.getOpacity());
        CGContextBeginTransparencyLayer(ctx, nullptr);
    }

    {
        auto painter = MacOSContext(ctx, true);
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

    drawImageInContext(ctx, view.renderNativeContent(scale), view.getLocalBounds());

    for (auto* child: view.getSubviews())
    {
        auto bounds = child->getBounds();

        CGContextSaveGState(ctx);
        CGContextTranslateCTM(ctx, bounds.x, bounds.y);
        CGContextClipToRect(ctx, CGRectMake(0, 0, bounds.w, bounds.h));
        compositeView(ctx, *child, scale);
        CGContextRestoreGState(ctx);
    }

    if (grouped)
        CGContextEndTransparencyLayer(ctx);
}

// Builds the off-screen bitmap and composites the view's synchronous content
// (paint, layers, GPU, child views) into it, leaving the flipped/scaled context
// open so the caller can overlay async web content before finalizing. Returns a
// retained context the caller owns (wrap in a CFRef or CFRelease it); CFRef is
// copy-unsafe, so ownership is passed as a raw handle rather than by value.
static CGContextRef makeCompositeContext(View& view, const Rect& bounds, float scale)
{
    auto pixelWidth = static_cast<int>(std::lround(bounds.w * scale));
    auto pixelHeight = static_cast<int>(std::lround(bounds.h * scale));

    if (pixelWidth <= 0 || pixelHeight <= 0)
        return nullptr;

    auto colorSpace = CFRef<CGColorSpaceRef>(CGColorSpaceCreateDeviceRGB());
    auto bitmapInfo = static_cast<std::uint32_t>(kCGImageAlphaPremultipliedLast)
                      | static_cast<std::uint32_t>(kCGBitmapByteOrder32Big);

    auto context = CGBitmapContextCreate(nullptr,
                                         static_cast<std::size_t>(pixelWidth),
                                         static_cast<std::size_t>(pixelHeight),
                                         8,
                                         0,
                                         colorSpace,
                                         bitmapInfo);
    if (context == nullptr)
        return nullptr;

    // paint() and the shape/text layers draw top-left origin to match the
    // flipped on-screen view; a bitmap context is bottom-left, so flip it, then
    // scale points to pixels for the backing scale.
    CGContextTranslateCTM(context, 0, pixelHeight);
    CGContextScaleCTM(context, 1.0, -1.0);
    CGContextScaleCTM(context, scale, scale);

    compositeView(context, view, scale);
    return context;
}

static Image imageFromContext(CGContextRef context)
{
    if (context == nullptr)
        return {};

    auto image = CFRef<CGImageRef>(CGBitmapContextCreateImage(context));
    if (!image)
        return {};

    auto error = std::string {};
    return detail::imageFromCGImage(image.get(), error);
}

Image renderLayerToImage(View& view, const Rect& bounds, float scale)
{
    auto context = CFRef<CGContextRef>(makeCompositeContext(view, bounds, scale));
    return imageFromContext(context.get());
}

namespace
{
struct AsyncTarget
{
    View* view;
    Point offset;
    float opacity;
};

// Collects descendant views with async (web) content, tagged with their origin
// in the root's coordinate space and the product of group opacities down to
// them, so each snapshot lands where -- and as faded as -- it sits on screen.
// (The synchronous composite applies group opacity via transparency layers; the
// async web overlay is drawn afterwards, so it must carry the opacity itself.)
void collectAsyncContent(View& view,
                         Point offset,
                         float opacity,
                         Vector<AsyncTarget>& out)
{
    auto effectiveOpacity = opacity * view.getOpacity();

    if (view.hasAsyncContent())
        out.push_back({&view, offset, effectiveOpacity});

    for (auto* child: view.getSubviews())
    {
        auto bounds = child->getBounds();
        collectAsyncContent(*child,
                            {offset.x + bounds.x, offset.y + bounds.y},
                            effectiveOpacity,
                            out);
    }
}

// Shared across the pending web snapshots: owns the open composite context and
// counts completions, resolving the promise once the last snapshot lands. The
// context outlives this call (it is drawn into from async callbacks), so it is
// owned here rather than in a copy-unsafe CFRef.
struct AsyncComposite
{
    ~AsyncComposite()
    {
        if (context != nullptr)
            CFRelease(context);
    }

    CGContextRef context = nullptr;
    Threads::AsyncPromise<Image> promise;
    int remaining = 0;
};
} // namespace

Threads::Async<Image>
    renderViewToImageAsync(View& view, const Rect& bounds, float scale)
{
    auto state = std::make_shared<AsyncComposite>();
    auto result = state->promise.get();

    state->context = makeCompositeContext(view, bounds, scale);
    if (state->context == nullptr)
    {
        state->promise.resolve({});
        return result;
    }

    auto targets = Vector<AsyncTarget> {};
    collectAsyncContent(view, {0.f, 0.f}, 1.0f, targets);

    if (targets.empty())
    {
        state->promise.resolve(imageFromContext(state->context));
        return result;
    }

    state->remaining = static_cast<int>(targets.size());

    for (auto& target: targets)
    {
        auto* webView = target.view;
        auto dest = Rect {target.offset.x,
                          target.offset.y,
                          webView->getBounds().w,
                          webView->getBounds().h};
        auto opacity = target.opacity;

        webView->captureAsyncContent(
            scale,
            [state, dest, opacity](Image webImage)
            {
                CGContextSaveGState(state->context);
                CGContextSetAlpha(state->context, opacity);
                drawImageInContext(state->context, webImage, dest);
                CGContextRestoreGState(state->context);

                if (--state->remaining == 0)
                    state->promise.resolve(imageFromContext(state->context));
            });
    }

    return result;
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

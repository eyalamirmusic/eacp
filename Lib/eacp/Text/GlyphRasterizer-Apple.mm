#import <CoreGraphics/CoreGraphics.h>
#import <CoreText/CoreText.h>

#include "GlyphRasterizer.h"

#include <eacp/Core/ObjC/CFRef.h>

#include <algorithm>
#include <cmath>

// CoreText rasterizer, shared by macOS and iOS — CoreText and CoreGraphics are
// present on both, so nothing here is macOS-specific. Draws each glyph into a bitmap sized to its own bounding
// box and reports where that box sits relative to the pen and baseline, rather
// than centring it in a fixed cell — that is what lets the atlas serve
// proportional text and ligatures, not just a monospace grid.

namespace eacp::Text
{
namespace
{
// Returns a +1 retained font; callers adopt it into a CFRef.
CTFontRef makeVariant(CTFontRef base, FontStyle style)
{
    const auto wanted = (CTFontSymbolicTraits) ((isBold(style) ? kCTFontTraitBold : 0)
                                                | (isItalic(style) ? kCTFontTraitItalic : 0));

    if (wanted != 0)
        if (auto* derived = CTFontCreateCopyWithSymbolicTraits(
                base, 0, nullptr, wanted, kCTFontTraitBold | kCTFontTraitItalic))
            return derived;

    return (CTFontRef) CFRetain(base);
}

int encodeUtf16(char32_t codepoint, UniChar* units)
{
    if (codepoint <= 0xffff)
    {
        units[0] = (UniChar) codepoint;
        return 1;
    }

    const auto value = codepoint - 0x10000;
    units[0] = (UniChar) (0xd800 + (value >> 10));
    units[1] = (UniChar) (0xdc00 + (value & 0x3ff));
    return 2;
}
} // namespace

struct GlyphRasterizer::Native
{
    explicit Native(const FontRequest& requestToUse)
        : request(requestToUse)
    {
        CFRef<CFStringRef> name(CFStringCreateWithCString(
            nullptr, request.family.c_str(), kCFStringEncodingUTF8));

        CFRef<CTFontRef> base(
            CTFontCreateWithName(name, request.pixelSize(), nullptr));

        if (!base)
            return;

        for (auto index = 0; index < 4; ++index)
            fonts[index].reset(makeVariant(base, (FontStyle) index));

        valid = fonts[0].get() != nullptr;
    }

    CTFontRef fontFor(FontStyle style) const
    {
        return fonts[(int) style].get() != nullptr ? fonts[(int) style].get()
                                                   : fonts[0].get();
    }

    FontMetrics metrics(FontStyle style) const
    {
        auto font = fontFor(style);
        auto result = FontMetrics {};

        if (font == nullptr)
            return result;

        result.ascent = (float) CTFontGetAscent(font);
        result.descent = (float) CTFontGetDescent(font);
        result.leading = (float) CTFontGetLeading(font);

        // 'M' is the conventional width probe; on a monospace face every glyph
        // shares this advance, and on a proportional one it is only a hint.
        const UniChar reference = 'M';
        auto glyph = CGGlyph {};

        if (CTFontGetGlyphsForCharacters(font, &reference, &glyph, 1))
            result.advance = (float) CTFontGetAdvancesForGlyphs(
                font, kCTFontOrientationHorizontal, &glyph, nullptr, 1);

        return result;
    }

    // Resolves the codepoint to a glyph, falling back to another face when the
    // requested one cannot draw it — how a Latin family still renders CJK and
    // emoji. Returns the font that owns the glyph.
    CFRef<CTFontRef> resolve(char32_t codepoint, FontStyle style, CGGlyph& glyph) const
    {
        UniChar units[2] = {};
        const auto unitCount = encodeUtf16(codepoint, units);

        auto base = fontFor(style);

        if (base == nullptr)
            return {};

        CGGlyph glyphs[2] = {};
        const auto direct =
            CTFontGetGlyphsForCharacters(base, units, glyphs, unitCount);

        auto font = CFRef<CTFontRef> {(CTFontRef) CFRetain(base)};

        if (!direct || glyphs[0] == 0)
        {
            CFRef<CFStringRef> text(
                CFStringCreateWithCharacters(nullptr, units, unitCount));

            if (auto* fallback =
                    CTFontCreateForString(base, text, CFRangeMake(0, unitCount)))
                font.reset(fallback);

            if (!CTFontGetGlyphsForCharacters(font, units, glyphs, unitCount)
                || glyphs[0] == 0)
                return {};
        }

        glyph = glyphs[0];
        return font;
    }

    GlyphBitmap rasterize(char32_t codepoint, FontStyle style) const
    {
        auto result = GlyphBitmap {};

        auto glyph = CGGlyph {};
        auto font = resolve(codepoint, style, glyph);

        if (!font)
            return result;

        result.valid = true;
        result.advance = (float) CTFontGetAdvancesForGlyphs(
            font, kCTFontOrientationHorizontal, &glyph, nullptr, 1);

        const auto colored =
            (CTFontGetSymbolicTraits(font) & kCTFontTraitColorGlyphs) != 0;

        result.format = colored ? GlyphFormat::Color : GlyphFormat::Mask;

        auto bounds = CTFontGetBoundingRectsForGlyphs(
            font, kCTFontOrientationHorizontal, &glyph, nullptr, 1);

        if (CGRectIsNull(bounds) || CGRectIsEmpty(bounds))
            return result; // valid but nothing to draw — a space

        // Snap the box outwards to whole pixels so antialiased edges are not
        // clipped, then rasterize at exactly that size.
        const auto left = (int) std::floor(bounds.origin.x);
        const auto bottom = (int) std::floor(bounds.origin.y);
        const auto right = (int) std::ceil(bounds.origin.x + bounds.size.width);
        const auto top = (int) std::ceil(bounds.origin.y + bounds.size.height);

        result.width = std::max(right - left, 1);
        result.height = std::max(top - bottom, 1);
        result.bearingX = (float) left;
        result.bearingY = (float) top;

        const auto stride = (std::size_t) result.width * bytesPerPixel(result.format);
        result.pixels.assign(stride * (std::size_t) result.height, 0);

        CFRef<CGColorSpaceRef> space(colored ? CGColorSpaceCreateDeviceRGB() : nullptr);

        // A mask needs one byte per pixel and no colour space: alpha-only is
        // exactly the coverage the atlas wants, with no channel to discard.
        CFRef<CGContextRef> context(CGBitmapContextCreate(
            result.pixels.data(),
            (std::size_t) result.width,
            (std::size_t) result.height,
            8,
            stride,
            colored ? space.get() : nullptr,
            colored ? kCGImageAlphaPremultipliedLast : kCGImageAlphaOnly));

        if (!context)
            return {};

        CGContextSetShouldAntialias(context, true);

        // Grayscale, never LCD: the atlas is tinted at draw time, and subpixel
        // antialiasing would bake one text colour into the cached coverage.
        CGContextSetShouldSmoothFonts(context, false);
        CGContextSetRGBFillColor(context, 1, 1, 1, 1);

        // Shift the glyph so its bounding box lands at the bitmap's origin.
        // CoreGraphics is y-up, so this is measured from the bottom.
        auto position = CGPointMake((CGFloat) -left, (CGFloat) -bottom);
        CTFontDrawGlyphs(font, &glyph, &position, 1, context);

        if (colored)
        {
            // The atlas stores straight alpha so a colour glyph can be blended
            // like any other; CoreGraphics hands back premultiplied.
            for (std::size_t i = 0; i + 3 < result.pixels.size(); i += 4)
            {
                const auto alpha = result.pixels[i + 3];

                if (alpha == 0 || alpha == 255)
                    continue;

                for (auto channel = 0; channel < 3; ++channel)
                    result.pixels[i + channel] = (std::uint8_t) std::min(
                        result.pixels[i + channel] * 255 / alpha, 255);
            }
        }

        return result;
    }

    FontRequest request;
    CFRef<CTFontRef> fonts[4];
    bool valid = false;
};

GlyphRasterizer::GlyphRasterizer(const FontRequest& request)
    : impl(request)
{
}

GlyphRasterizer::~GlyphRasterizer() = default;

bool GlyphRasterizer::isValid() const
{
    return impl->valid;
}

FontMetrics GlyphRasterizer::metrics(FontStyle style) const
{
    return impl->metrics(style);
}

float GlyphRasterizer::scale() const
{
    return impl->request.scale;
}

GlyphBitmap GlyphRasterizer::rasterize(char32_t codepoint, FontStyle style) const
{
    return impl->rasterize(codepoint, style);
}

const FontRequest& GlyphRasterizer::request() const
{
    return impl->request;
}

bool registerMemoryFont(const void* data, std::size_t size)
{
    if (data == nullptr || size == 0)
        return false;

    CFRef<CFDataRef> wrapped(CFDataCreateWithBytesNoCopy(
        nullptr, (const UInt8*) data, (CFIndex) size, kCFAllocatorNull));

    if (!wrapped)
        return false;

    CFRef<CGDataProviderRef> provider(CGDataProviderCreateWithCFData(wrapped));
    CFRef<CGFontRef> font(CGFontCreateWithDataProvider(provider));

    if (!font)
        return false;

    // Deprecated, but the only API that registers an in-memory font for
    // process-wide name lookup — the replacements want file URLs, and spilling
    // an embedded font to disk to load it back defeats the point of embedding.
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
    const auto registered = CTFontManagerRegisterGraphicsFont(font, nullptr);
#pragma clang diagnostic pop

    return registered == true;
}
} // namespace eacp::Text

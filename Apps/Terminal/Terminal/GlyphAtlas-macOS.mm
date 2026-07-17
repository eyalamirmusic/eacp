#import <AppKit/AppKit.h>
#import <CoreText/CoreText.h>

#include "GlyphAtlas.h"
#include "TermTypes.h"

#include <eacp/Core/ObjC/CFRef.h>

#include <ResEmbed/ResEmbed.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <optional>
#include <unordered_map>
#include <vector>

namespace term
{
using namespace eacp;

namespace
{
constexpr int atlasSize = 2048;
constexpr int slotPadding = 1;

// Returns a +1 retained font; callers adopt it into a CFRef with reset().
CTFontRef makeVariant(CTFontRef base, bool bold, bool italic)
{
    const auto wanted = (CTFontSymbolicTraits) (
        (bold ? kCTFontTraitBold : 0) | (italic ? kCTFontTraitItalic : 0));

    if (wanted != 0)
        if (auto* derived = CTFontCreateCopyWithSymbolicTraits(
                base, 0, nullptr, wanted,
                kCTFontTraitBold | kCTFontTraitItalic))
            return derived;

    return (CTFontRef) CFRetain(base);
}

std::uint32_t slotKey(char32_t cp, bool bold, bool italic)
{
    return (std::uint32_t) cp | (bold ? 1u << 22 : 0)
           | (italic ? 1u << 23 : 0);
}

int encodeUtf16(char32_t cp, UniChar* units)
{
    if (cp <= 0xffff)
    {
        units[0] = (UniChar) cp;
        return 1;
    }

    const auto v = cp - 0x10000;
    units[0] = (UniChar) (0xd800 + (v >> 10));
    units[1] = (UniChar) (0xdc00 + (v & 0x3ff));
    return 2;
}
} // namespace

void registerEmbeddedFonts()
{
    static const auto once = []
    {
        for (const auto* name: {"JetBrainsMono-Regular.ttf",
                                "JetBrainsMono-Bold.ttf",
                                "JetBrainsMono-Italic.ttf",
                                "JetBrainsMono-BoldItalic.ttf"})
        {
            const auto resource = ResEmbed::get(name);

            if (resource.size() == 0)
                continue;

            CFRef<CFDataRef> data(
                CFDataCreateWithBytesNoCopy(nullptr,
                                            resource.data(),
                                            (CFIndex) resource.size(),
                                            kCFAllocatorNull));
            CFRef<CGDataProviderRef> provider(
                CGDataProviderCreateWithCFData(data));
            CFRef<CGFontRef> font(CGFontCreateWithDataProvider(provider));

            // Deprecated, but it is the only API that registers an
            // in-memory font for process-wide name lookup — the suggested
            // replacements want file URLs, and spilling the embedded font
            // to disk just to load it back defeats the point.
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
            if (font)
                CTFontManagerRegisterGraphicsFont(font, nullptr);
#pragma clang diagnostic pop
        }

        return true;
    }();

    (void) once;
}

struct GlyphAtlas::Impl
{
    Impl(const std::string& fontName, float pointSizeToUse, float scaleToUse)
    {
        scale = scaleToUse > 0 ? scaleToUse
                               : (float) NSScreen.mainScreen.backingScaleFactor;

        if (scale <= 0)
            scale = 2.0f;

        pointSize = pointSizeToUse;

        CFRef<CFStringRef> name(CFStringCreateWithCString(
            nullptr, fontName.c_str(), kCFStringEncodingUTF8));
        CFRef<CTFontRef> base(
            CTFontCreateWithName(name, pointSize * scale, nullptr));

        fonts[0].reset(makeVariant(base, false, false));
        fonts[1].reset(makeVariant(base, true, false));
        fonts[2].reset(makeVariant(base, false, true));
        fonts[3].reset(makeVariant(base, true, true));

        const UniChar reference = 'M';
        auto glyph = CGGlyph {};
        CTFontGetGlyphsForCharacters(fonts[0], &reference, &glyph, 1);
        const auto advance = CTFontGetAdvancesForGlyphs(
            fonts[0], kCTFontOrientationHorizontal, &glyph, nullptr, 1);

        ascentPx = (float) CTFontGetAscent(fonts[0]);
        const auto descent = (float) CTFontGetDescent(fonts[0]);
        const auto leading = (float) CTFontGetLeading(fonts[0]);

        cellPxW = (int) std::ceil(advance);
        cellPxH = (int) std::ceil(ascentPx + descent + leading);

        pixels.assign((std::size_t) atlasSize * atlasSize * 4, 0);
    }

    const GlyphSlot& glyph(char32_t cp, bool bold, bool italic)
    {
        const auto key = slotKey(cp, bold, italic);
        auto found = slots.find(key);

        if (found != slots.end())
            return found->second;

        return slots.emplace(key, rasterize(cp, bold, italic)).first->second;
    }

    GlyphSlot rasterize(char32_t cp, bool bold, bool italic)
    {
        UniChar units[2] = {};
        const auto unitCount = encodeUtf16(cp, units);

        CTFontRef base = fonts[(bold ? 1 : 0) + (italic ? 2 : 0)];
        CGGlyph glyphs[2] = {};
        auto hasGlyph = CTFontGetGlyphsForCharacters(
            base, units, glyphs, unitCount);

        auto drawFont = CFRef<CTFontRef> {(CTFontRef) CFRetain(base)};

        if (!hasGlyph || glyphs[0] == 0)
        {
            CFRef<CFStringRef> text(
                CFStringCreateWithCharacters(nullptr, units, unitCount));

            if (auto* fallback = CTFontCreateForString(
                    base, text, CFRangeMake(0, unitCount)))
                drawFont.reset(fallback);

            if (!CTFontGetGlyphsForCharacters(
                    drawFont, units, glyphs, unitCount)
                || glyphs[0] == 0)
                return {};
        }

        const auto colored =
            (CTFontGetSymbolicTraits(drawFont) & kCTFontTraitColorGlyphs) != 0;

        const auto cells = charWidth(cp) == 2 ? 2 : 1;
        const auto slotW = cellPxW * cells;
        const auto slotH = cellPxH;

        if (penX + slotW + slotPadding > atlasSize)
        {
            penX = slotPadding;
            penY += slotH + slotPadding;
        }

        if (penY + slotH + slotPadding > atlasSize)
            resetAtlas();

        auto buffer = std::vector<std::uint8_t>(
            (std::size_t) slotW * (std::size_t) slotH * 4, 0);

        CFRef<CGColorSpaceRef> space(CGColorSpaceCreateDeviceRGB());
        CFRef<CGContextRef> ctx(CGBitmapContextCreate(
            buffer.data(),
            (std::size_t) slotW,
            (std::size_t) slotH,
            8,
            (std::size_t) slotW * 4,
            space,
            kCGImageAlphaPremultipliedLast));

        if (!ctx)
            return {};

        CGContextSetShouldAntialias(ctx, true);
        CGContextSetShouldSmoothFonts(ctx, false);
        CGContextSetRGBFillColor(ctx, 1, 1, 1, 1);

        const auto advance = CTFontGetAdvancesForGlyphs(
            drawFont, kCTFontOrientationHorizontal, glyphs, nullptr, 1);
        const auto x = std::max(((double) slotW - advance) / 2.0, 0.0);
        const auto baselineFromBottom = (double) slotH - (double) ascentPx;

        auto position = CGPointMake(x, baselineFromBottom);
        CTFontDrawGlyphs(drawFont, glyphs, &position, 1, ctx);

        blitToAtlas(buffer, slotW, slotH, colored);

        auto slot = GlyphSlot {};
        slot.src = {(float) penX, (float) penY, (float) slotW, (float) slotH};
        slot.colored = colored;
        slot.valid = true;

        penX += slotW + slotPadding;
        dirty = true;
        return slot;
    }

    // Converts the premultiplied CG bitmap to the straight-alpha atlas.
    // Monochrome coverage becomes white + alpha for per-cell tinting.
    void blitToAtlas(const std::vector<std::uint8_t>& buffer,
                     int slotW,
                     int slotH,
                     bool colored)
    {
        for (auto y = 0; y < slotH; ++y)
        {
            const auto* src = &buffer[(std::size_t) y * (std::size_t) slotW * 4];
            auto* dst = &pixels[((std::size_t) (penY + y) * atlasSize
                                 + (std::size_t) penX)
                                * 4];

            for (auto x = 0; x < slotW; ++x)
            {
                const auto alpha = src[x * 4 + 3];

                if (colored && alpha > 0)
                {
                    dst[x * 4 + 0] =
                        (std::uint8_t) std::min(src[x * 4 + 0] * 255 / alpha, 255);
                    dst[x * 4 + 1] =
                        (std::uint8_t) std::min(src[x * 4 + 1] * 255 / alpha, 255);
                    dst[x * 4 + 2] =
                        (std::uint8_t) std::min(src[x * 4 + 2] * 255 / alpha, 255);
                }
                else
                {
                    dst[x * 4 + 0] = 255;
                    dst[x * 4 + 1] = 255;
                    dst[x * 4 + 2] = 255;
                }

                dst[x * 4 + 3] = alpha;
            }
        }
    }

    void resetAtlas()
    {
        std::fill(pixels.begin(), pixels.end(), 0);
        slots.clear();
        penX = slotPadding;
        penY = slotPadding;
        dirty = true;
    }

    GPU::Texture& texture()
    {
        if (!tex)
        {
            auto descriptor = GPU::TextureDescriptor {};
            descriptor.width = atlasSize;
            descriptor.height = atlasSize;
            descriptor.format = GPU::TextureFormat::RGBA8Unorm;
            descriptor.filter = GPU::TextureFilter::Linear;
            tex.emplace(
                GPU::Device::shared().makeTexture(descriptor, pixels.data()));
            dirty = false;
        }
        else if (dirty)
        {
            tex->update(pixels.data());
            dirty = false;
        }

        return *tex;
    }

    float scale = 2;
    float pointSize = 13;
    float ascentPx = 0;
    int cellPxW = 0;
    int cellPxH = 0;
    int penX = slotPadding;
    int penY = slotPadding;
    bool dirty = true;

    CFRef<CTFontRef> fonts[4];
    std::vector<std::uint8_t> pixels;
    std::unordered_map<std::uint32_t, GlyphSlot> slots;
    std::optional<GPU::Texture> tex;
};

GlyphAtlas::GlyphAtlas(const std::string& fontName, float pointSize, float scale)
    : impl(fontName, pointSize, scale)
{
}

GlyphAtlas::~GlyphAtlas() = default;

float GlyphAtlas::cellWidth() const
{
    return (float) impl->cellPxW / impl->scale;
}

float GlyphAtlas::cellHeight() const
{
    return (float) impl->cellPxH / impl->scale;
}

float GlyphAtlas::baseline() const
{
    return impl->ascentPx / impl->scale;
}

float GlyphAtlas::fontSize() const
{
    return impl->pointSize;
}

const GlyphSlot& GlyphAtlas::glyph(char32_t cp, bool bold, bool italic)
{
    return impl->glyph(cp, bold, italic);
}

GPU::Texture& GlyphAtlas::texture()
{
    return impl->texture();
}
} // namespace term

#import <CoreGraphics/CoreGraphics.h>
#import <CoreText/CoreText.h>

#include "GlyphRasterizer.h"
#include "Utf8.h"

#include <eacp/Core/ObjC/CFRef.h>

#include <algorithm>
#include <cmath>
#include <map>
#include <vector>

// CoreText rasterizer and shaper, shared by macOS and iOS — CoreText and
// CoreGraphics are present on both, so nothing here is macOS-specific.
//
// Shaping goes through CTLine, which kerns, ligates, places marks and falls
// back to other faces for what the family lacks, and hands back runs of glyph
// ids with positions. Each glyph is then drawn into a bitmap sized to its own
// bounding box, with where that box sits relative to the pen and baseline,
// rather than centred in a fixed cell.

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

// CoreText's weight trait is a number from -1 to 1 on a scale of its own;
// these are the values its named weights sit at, in CSS's order.
struct WeightAnchor
{
    float trait;
    int css;
};

constexpr WeightAnchor weightAnchors[] = {{-0.8f, 100},
                                          {-0.6f, 200},
                                          {-0.4f, 300},
                                          {0.0f, 400},
                                          {0.23f, 500},
                                          {0.3f, 600},
                                          {0.4f, 700},
                                          {0.56f, 800},
                                          {0.62f, 900}};

int cssWeightOf(float trait)
{
    auto best = weightAnchors[0];

    for (const auto& anchor: weightAnchors)
        if (std::abs(anchor.trait - trait) < std::abs(best.trait - trait))
            best = anchor;

    return best.css;
}

// How far a face's weight is from the one wanted, by CSS Fonts' matching:
// from 400 the search goes to 500 first, then downwards, then up; from 500
// to 400 first, then down, then up; below 400 downwards then up; above 500
// upwards then down. Smaller is nearer.
int weightDistance(int wanted, int have)
{
    if (have == wanted)
        return 0;

    const auto above = have > wanted;

    if (wanted == 400 && have == 500)
        return 1;

    if (wanted == 500 && have == 400)
        return 1;

    if (wanted <= 500)
        return above ? 1000 + (have - wanted) : 2 + (wanted - have);

    return above ? 2 + (have - wanted) : 1000 + (wanted - have);
}

// UTF-8 to UTF-16, remembering which byte each code unit came from, so a
// glyph's string index maps back to an offset in the text the caller gave.
struct Utf16Text
{
    std::vector<UniChar> units;
    std::vector<int> byteOf;
};

Utf16Text toUtf16(std::string_view text)
{
    auto result = Utf16Text {};
    result.units.reserve(text.size());
    result.byteOf.reserve(text.size() + 1);

    auto index = std::size_t {0};

    while (index < text.size())
    {
        const auto start = index;
        const auto codepoint = decodeUtf8(text, index);

        if (codepoint <= 0xFFFF)
        {
            result.units.push_back((UniChar) codepoint);
            result.byteOf.push_back((int) start);
        }
        else
        {
            const auto value = codepoint - 0x10000;
            result.units.push_back((UniChar) (0xD800 + (value >> 10)));
            result.units.push_back((UniChar) (0xDC00 + (value & 0x3FF)));
            result.byteOf.push_back((int) start);
            result.byteOf.push_back((int) start);
        }
    }

    result.byteOf.push_back((int) text.size());

    return result;
}

std::string toString(CFStringRef string)
{
    if (string == nullptr)
        return {};

    auto capacity = CFStringGetMaximumSizeForEncoding(CFStringGetLength(string),
                                                      kCFStringEncodingUTF8)
                    + 1;
    auto buffer = std::string(static_cast<std::size_t>(capacity), '\0');

    if (!CFStringGetCString(string, buffer.data(), capacity, kCFStringEncodingUTF8))
        return {};

    return buffer.c_str();
}
} // namespace

struct GlyphRasterizer::Native
{
    explicit Native(const FontRequest& requestToUse)
        : request(requestToUse)
    {
        CFRef<CFStringRef> name(CFStringCreateWithCString(
            nullptr, request.family.c_str(), kCFStringEncodingUTF8));

        base.reset(CTFontCreateWithName(name, request.pixelSize(), nullptr));

        if (!base)
            return;

        valid = true;
        resolved = familyNameOf(base.get());
        collectFamilyFaces();
    }

    static std::string familyNameOf(CTFontRef font)
    {
        CFRef<CFStringRef> name(CTFontCopyFamilyName(font));

        return toString(name.get());
    }

    // One face of the family: its descriptor and where it sits on the axes.
    // The width is kept because a family's heaviest faces are often its
    // condensed ones, and CSS matches by stretch before weight.
    struct FamilyFace
    {
        CFRef<CTFontDescriptorRef> descriptor;
        int weight = 400;
        float width = 0.f;
        bool italic = false;
    };

    // Every face the family has, read once, so a variant is matched to a real
    // face by CSS's rules rather than to whatever CoreText's trait matching
    // considers close enough.
    void collectFamilyFaces()
    {
        CFRef<CFStringRef> family(CFStringCreateWithCString(
            nullptr, resolved.c_str(), kCFStringEncodingUTF8));

        const void* keys[] = {kCTFontFamilyNameAttribute};
        const void* values[] = {family.get()};

        CFRef<CFDictionaryRef> attributes(
            CFDictionaryCreate(nullptr,
                               keys,
                               values,
                               1,
                               &kCFTypeDictionaryKeyCallBacks,
                               &kCFTypeDictionaryValueCallBacks));

        CFRef<CTFontDescriptorRef> wanted(
            CTFontDescriptorCreateWithAttributes(attributes));
        CFRef<CFArrayRef> matches(
            CTFontDescriptorCreateMatchingFontDescriptors(wanted, nullptr));

        if (!matches)
            return;

        for (auto index = CFIndex {0}; index < CFArrayGetCount(matches); ++index)
        {
            auto descriptor = (CTFontDescriptorRef) CFArrayGetValueAtIndex(matches, index);
            CFRef<CFDictionaryRef> traits(
                (CFDictionaryRef) CTFontDescriptorCopyAttribute(descriptor,
                                                                kCTFontTraitsAttribute));

            auto face = FamilyFace {};
            face.descriptor.reset((CTFontDescriptorRef) CFRetain(descriptor));

            if (traits)
            {
                if (auto weight = (CFNumberRef) CFDictionaryGetValue(traits,
                                                                     kCTFontWeightTrait))
                {
                    auto value = 0.f;
                    CFNumberGetValue(weight, kCFNumberFloatType, &value);
                    face.weight = cssWeightOf(value);
                }

                if (auto width = (CFNumberRef) CFDictionaryGetValue(traits,
                                                                    kCTFontWidthTrait))
                    CFNumberGetValue(width, kCFNumberFloatType, &face.width);

                if (auto symbolic = (CFNumberRef) CFDictionaryGetValue(
                        traits, kCTFontSymbolicTrait))
                {
                    auto value = std::uint32_t {0};
                    CFNumberGetValue(symbolic, kCFNumberSInt32Type, &value);
                    face.italic = (value & kCTFontTraitItalic) != 0;
                }
            }

            familyFaces.push_back(std::move(face));
        }
    }

    // The family's face nearest the variant, by CSS's matching: the normal
    // width first, then the slant, then the weight. Null when the family
    // reported no faces.
    const FamilyFace* nearestFace(const FontVariant& variant) const
    {
        const FamilyFace* best = nullptr;
        auto bestDistance = 0.f;

        for (const auto& face: familyFaces)
        {
            const auto distance = std::abs(face.width) * 1000000.f
                                  + (face.italic != variant.italic ? 10000.f : 0.f)
                                  + (float) weightDistance(variant.weight, face.weight);

            if (best == nullptr || distance < bestDistance)
            {
                best = &face;
                bestDistance = distance;
            }
        }

        return best;
    }

    CTFontRef fontFor(const FontVariant& variant) const
    {
        if (!base)
            return nullptr;

        const auto key = weightClass(variant.weight) * 2 + (variant.italic ? 1 : 0);
        auto found = variants.find(key);

        if (found != variants.end())
            return found->second.get();

        auto font = CFRef<CTFontRef> {makeFont(variant)};
        auto* result = font.get();
        variants.emplace(key, std::move(font));

        return result;
    }

    // Returns a +1 retained font: the family's nearest face, given the traits
    // it lacks synthetically where CoreText can, and the base face when the
    // family could not be enumerated at all.
    CTFontRef makeFont(const FontVariant& variant) const
    {
        const auto* face = nearestFace(variant);

        if (face == nullptr)
            return makeVariant(base.get(), styleOf(variant));

        auto font = CFRef<CTFontRef> {CTFontCreateWithFontDescriptor(
            face->descriptor.get(), request.pixelSize(), nullptr)};

        if (!font)
            return makeVariant(base.get(), styleOf(variant));

        const auto missingBold = variant.weight >= 600 && face->weight < 600;
        const auto missingItalic = variant.italic && !face->italic;
        const auto wanted = (CTFontSymbolicTraits) ((missingBold ? kCTFontTraitBold : 0)
                                                    | (missingItalic ? kCTFontTraitItalic : 0));

        if (wanted != 0)
            if (auto* derived = CTFontCreateCopyWithSymbolicTraits(
                    font.get(), 0, nullptr, wanted, wanted))
                return derived;

        return (CTFontRef) CFRetain(font.get());
    }

    FontMetrics metrics(const FontVariant& variant) const
    {
        auto font = fontFor(variant);
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

    // The number a run's font shapes under: 0 for the face asked for, else
    // the fallback's place in the table, added the first time it is met.
    int fontIndexOf(CTFontRef runFont, CTFontRef requested) const
    {
        if (runFont == nullptr || CFEqual(runFont, requested))
            return 0;

        for (auto index = std::size_t {0}; index < fallbacks.size(); ++index)
            if (CFEqual(fallbacks[index].get(), runFont))
                return (int) index + 1;

        if (fallbacks.size() >= 255)
            return 0;

        fallbacks.emplace_back((CTFontRef) CFRetain(runFont));

        return (int) fallbacks.size();
    }

    CTFontRef fontOf(GlyphKey key, const FontVariant& variant) const
    {
        if (key.font == 0)
            return fontFor(variant);

        const auto index = (std::size_t) key.font - 1;

        return index < fallbacks.size() ? fallbacks[index].get() : nullptr;
    }

    ShapedRun shape(std::string_view text, const FontVariant& variant) const
    {
        auto result = ShapedRun {};
        auto font = fontFor(variant);

        if (font == nullptr || text.empty())
            return result;

        const auto utf16 = toUtf16(text);

        CFRef<CFStringRef> string(CFStringCreateWithCharacters(
            nullptr, utf16.units.data(), (CFIndex) utf16.units.size()));

        const void* keys[] = {kCTFontAttributeName};
        const void* values[] = {font};

        CFRef<CFDictionaryRef> attributes(
            CFDictionaryCreate(nullptr,
                               keys,
                               values,
                               1,
                               &kCFTypeDictionaryKeyCallBacks,
                               &kCFTypeDictionaryValueCallBacks));

        CFRef<CFAttributedStringRef> attributed(
            CFAttributedStringCreate(nullptr, string, attributes));
        CFRef<CTLineRef> line(CTLineCreateWithAttributedString(attributed));

        if (!line)
            return result;

        auto runs = CTLineGetGlyphRuns(line);

        for (auto runIndex = CFIndex {0}; runIndex < CFArrayGetCount(runs); ++runIndex)
        {
            auto run = (CTRunRef) CFArrayGetValueAtIndex(runs, runIndex);
            const auto count = CTRunGetGlyphCount(run);

            if (count <= 0)
                continue;

            auto glyphs = std::vector<CGGlyph>((std::size_t) count);
            auto positions = std::vector<CGPoint>((std::size_t) count);
            auto indices = std::vector<CFIndex>((std::size_t) count);

            CTRunGetGlyphs(run, CFRangeMake(0, 0), glyphs.data());
            CTRunGetPositions(run, CFRangeMake(0, 0), positions.data());
            CTRunGetStringIndices(run, CFRangeMake(0, 0), indices.data());

            auto runFont = (CTFontRef) CFDictionaryGetValue(CTRunGetAttributes(run),
                                                            kCTFontAttributeName);
            const auto fontIndex = fontIndexOf(runFont, font);

            for (auto i = std::size_t {0}; i < glyphs.size(); ++i)
            {
                const auto unit = std::clamp(
                    (std::size_t) indices[i], std::size_t {0}, utf16.byteOf.size() - 1);

                result.glyphs.add({{glyphs[i], fontIndex},
                                   (float) positions[i].x,
                                   (float) positions[i].y,
                                   utf16.byteOf[unit]});
            }
        }

        result.advance = (float) CTLineGetTypographicBounds(line, nullptr, nullptr, nullptr);

        return result;
    }

    GlyphBitmap rasterize(GlyphKey key,
                          const FontVariant& variant,
                          const RasterRequest& request) const
    {
        auto result = GlyphBitmap {};
        auto font = fontOf(key, variant);

        if (font == nullptr)
            return result;

        auto glyph = (CGGlyph) key.glyph;

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
        // clipped, then rasterize at exactly that size — the box of the glyph
        // as it is drawn, shifted right by the subpixel offset, so a stem that
        // crosses a pixel edge at this phase has its extra column. One more
        // column each side for the font smoothing, which thickens a stem by
        // about a third of a pixel beyond the outline's own bounds and
        // nothing above or below it.
        const auto shift = (CGFloat) std::clamp(request.subpixelX, 0.f, 1.f);
        const auto left = (int) std::floor(bounds.origin.x + shift) - 1;
        const auto bottom = (int) std::floor(bounds.origin.y);
        const auto right =
            (int) std::ceil(bounds.origin.x + shift + bounds.size.width) + 1;
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

        // Grayscale, with the platform's font smoothing on. Since Mojave there
        // is no LCD antialiasing left for smoothing to mean, so nothing is
        // baked into the coverage that a tint at draw time could disagree
        // with; what it means now is the slight thickening of every stem the
        // system's own text is drawn with, and an alpha-only context applies
        // it as an RGB one would.
        CGContextSetAllowsFontSmoothing(context, true);
        CGContextSetShouldSmoothFonts(context, true);

        // The subpixel offset is meant exactly; left to itself CoreGraphics
        // quantizes a fractional position to steps of its own choosing.
        CGContextSetAllowsFontSubpixelPositioning(context, true);
        CGContextSetShouldSubpixelPositionFonts(context, true);
        CGContextSetAllowsFontSubpixelQuantization(context, false);
        CGContextSetShouldSubpixelQuantizeFonts(context, false);

        // The fill's lightness is what the smoothing reads to decide how much
        // to thicken the stems, light text getting more; an alpha-only context
        // keeps no colour, but the decision is made before that.
        const auto ink = (CGFloat) (request.lightText ? 1 : 0);
        CGContextSetRGBFillColor(context, ink, ink, ink, 1);

        // Shift the glyph so its bounding box lands at the bitmap's origin,
        // then right by the subpixel offset. CoreGraphics is y-up, so the
        // vertical shift is measured from the bottom.
        auto position = CGPointMake((CGFloat) -left + shift, (CGFloat) -bottom);
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
    CFRef<CTFontRef> base;
    std::vector<FamilyFace> familyFaces;
    mutable std::map<int, CFRef<CTFontRef>> variants;
    mutable std::vector<CFRef<CTFontRef>> fallbacks;
    bool valid = false;
    std::string resolved;
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

std::string GlyphRasterizer::resolvedFamily() const
{
    return impl->resolved;
}

FontMetrics GlyphRasterizer::metrics(const FontVariant& variant) const
{
    return impl->metrics(variant);
}

float GlyphRasterizer::scale() const
{
    return impl->request.scale;
}

ShapedRun GlyphRasterizer::shape(std::string_view text, const FontVariant& variant) const
{
    return impl->shape(text, variant);
}

GlyphBitmap GlyphRasterizer::rasterize(GlyphKey glyph,
                                       const FontVariant& variant,
                                       const RasterRequest& request) const
{
    return impl->rasterize(glyph, variant, request);
}

GlyphBitmap GlyphRasterizer::rasterize(char32_t codepoint, FontStyle style) const
{
    char encoded[4] = {};
    const auto length = encodeUtf8(codepoint, encoded);

    const auto variant = variantOf(style);
    const auto run = impl->shape({encoded, length}, variant);

    if (run.glyphs.empty())
        return {};

    return impl->rasterize(run.glyphs[0].key, variant, {});
}

const FontRequest& GlyphRasterizer::request() const
{
    return impl->request;
}

namespace
{
// Whether a face by that PostScript name resolves to itself. CoreText draws
// something for any name, so the name of what came back is the answer.
bool faceResolves(const std::string& postScriptName)
{
    CFRef<CFStringRef> name(CFStringCreateWithCString(
        nullptr, postScriptName.c_str(), kCFStringEncodingUTF8));
    CFRef<CTFontRef> font(CTFontCreateWithName(name, 12.f, nullptr));

    if (!font)
        return false;

    CFRef<CFStringRef> resolved(CTFontCopyPostScriptName(font));

    return toString(resolved.get()) == postScriptName;
}
} // namespace

std::optional<RegisteredFont> registerMemoryFont(const void* data, std::size_t size)
{
    if (data == nullptr || size == 0)
        return std::nullopt;

    CFRef<CFDataRef> copied(CFDataCreate(nullptr, (const UInt8*) data, (CFIndex) size));

    if (!copied)
        return std::nullopt;

    CFRef<CGDataProviderRef> provider(CGDataProviderCreateWithCFData(copied));
    CFRef<CGFontRef> font(CGFontCreateWithDataProvider(provider));

    if (!font)
        return std::nullopt;

    auto names = RegisteredFont {};
    CFRef<CFStringRef> postScriptName(CGFontCopyPostScriptName(font));
    names.postScriptName = toString(postScriptName.get());

    CFRef<CTFontRef> face(CTFontCreateWithGraphicsFont(font, 12.f, nullptr, nullptr));

    if (face)
    {
        CFRef<CFStringRef> family(CTFontCopyFamilyName(face));
        names.family = toString(family.get());
    }

    if (names.family.empty() || names.postScriptName.empty())
        return std::nullopt;

    // Deprecated, but the only API that registers an in-memory font for
    // process-wide name lookup — the replacements want file URLs, and spilling
    // an embedded font to disk to load it back defeats the point of embedding.
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
    const auto registered = CTFontManagerRegisterGraphicsFont(font, nullptr);
#pragma clang diagnostic pop

    // A registration refused is usually this face registered earlier - a
    // page reloaded - and the face is there whichever call put it there.
    if (!registered && !faceResolves(names.postScriptName))
        return std::nullopt;

    return names;
}
} // namespace eacp::Text

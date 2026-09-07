#include "GlyphRasterizer.h"
#include "Utf8.h"

#include <eacp/Core/Utils/Strings.h>

#include <ft2build.h>
#include FT_FREETYPE_H
#include <freetype/ftmm.h>
#include <freetype/ftoutln.h>
#include <freetype/tttables.h>

#include <fontconfig/fontconfig.h>

#include <hb.h>
#include <hb-ft.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <utility>

namespace eacp::Text
{
// Named rather than anonymous: Native holds these as members in a header, and
// GCC reads an internal-linkage member as an ODR hazard (-Wsubobject-linkage).
namespace LinuxText
{
struct LinuxFaceDeleter
{
    void operator()(FT_FaceRec_* face) const { FT_Done_Face(face); }
};

struct LinuxShaperFontDeleter
{
    void operator()(hb_font_t* font) const { hb_font_destroy(font); }
};

struct LinuxShaperFaceDeleter
{
    void operator()(hb_face_t* face) const { hb_face_destroy(face); }
};

struct LinuxShaperBufferDeleter
{
    void operator()(hb_buffer_t* buffer) const { hb_buffer_destroy(buffer); }
};

struct LinuxPatternDeleter
{
    void operator()(FcPattern* pattern) const { FcPatternDestroy(pattern); }
};

struct LinuxFontSetDeleter
{
    void operator()(FcFontSet* set) const { FcFontSetDestroy(set); }
};

struct LinuxObjectSetDeleter
{
    void operator()(FcObjectSet* set) const { FcObjectSetDestroy(set); }
};

using LinuxFace = std::unique_ptr<FT_FaceRec_, LinuxFaceDeleter>;
using LinuxShaperFont = std::unique_ptr<hb_font_t, LinuxShaperFontDeleter>;
using LinuxShaperFace = std::unique_ptr<hb_face_t, LinuxShaperFaceDeleter>;
using LinuxShaperBuffer = std::unique_ptr<hb_buffer_t, LinuxShaperBufferDeleter>;
using LinuxPattern = std::unique_ptr<FcPattern, LinuxPatternDeleter>;
using LinuxFontSet = std::unique_ptr<FcFontSet, LinuxFontSetDeleter>;
using LinuxObjectSet = std::unique_ptr<FcObjectSet, LinuxObjectSetDeleter>;

// The bytes live for the life of the process: every FT_Face opened over them
// reads out of them lazily.
struct LinuxMemoryFont
{
    Vector<std::uint8_t> bytes;
    std::string family;
    std::string postScriptName;
    int weight = 400;
    bool italic = false;
    bool weightVaries = false;
};

// One FT_Library and one fontconfig for the process. Neither FreeType nor
// HarfBuzz's ft funcs are thread safe, so every call into them takes the mutex.
struct LinuxFontSystem
{
    LinuxFontSystem()
    {
        ready = FT_Init_FreeType(&library) == 0 && FcInit() == FcTrue;
    }

    std::mutex mutex;
    FT_Library library = nullptr;
    Vector<std::unique_ptr<LinuxMemoryFont>> memoryFonts;
    bool ready = false;
};

LinuxFontSystem& fontSystem()
{
    static auto* instance = new LinuxFontSystem {};

    return *instance;
}

constexpr FT_ULong linuxAxisTag(char a, char b, char c, char d)
{
    return ((FT_ULong) a << 24) | ((FT_ULong) b << 16) | ((FT_ULong) c << 8)
           | (FT_ULong) d;
}

constexpr auto linuxWeightAxis = linuxAxisTag('w', 'g', 'h', 't');
constexpr auto linuxItalicAxis = linuxAxisTag('i', 't', 'a', 'l');
constexpr auto linuxSlantAxis = linuxAxisTag('s', 'l', 'n', 't');
constexpr auto linuxOpticalSizeAxis = linuxAxisTag('o', 'p', 's', 'z');

// CSS oblique on the slnt axis, which counts degrees anticlockwise from upright.
constexpr float linuxObliqueSlant = -14.f;

// The same lean as a shear: tan(14°).
constexpr float linuxObliqueShear = 0.24933f;

// Fraction of the em; what HarfBuzz measures and FreeType draws share it.
constexpr float linuxSyntheticBoldRatio = 0.02f;

constexpr int linuxNormalWidth = 100;

// CSS Fonts' weight matching, smaller being nearer. The same rule as
// GlyphRasterizer-Apple.mm's weightDistance, and it has to stay the same rule.
int linuxWeightDistance(int wanted, int have)
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

struct LinuxFamilyFace
{
    std::string file;
    int faceIndex = 0;
    const LinuxMemoryFont* memory = nullptr;
    int weight = 400;
    int width = linuxNormalWidth;
    bool italic = false;

    bool weightVaries = false;

    bool isEmpty() const { return file.empty() && memory == nullptr; }
};

const LinuxFamilyFace* linuxNearestFace(const Vector<LinuxFamilyFace>& faces,
                                        const FontVariant& variant)
{
    const LinuxFamilyFace* best = nullptr;
    auto bestDistance = 0.f;

    for (const auto& face: faces)
    {
        const auto weight =
            face.weightVaries ? 0 : linuxWeightDistance(variant.weight, face.weight);

        const auto distance =
            (float) std::abs(face.width - linuxNormalWidth) * 1000000.f
            + (face.italic != variant.italic ? 10000.f : 0.f) + (float) weight;

        if (best == nullptr || distance < bestDistance)
        {
            best = &face;
            bestDistance = distance;
        }
    }

    return best;
}

std::string linuxToString(const FcChar8* text)
{
    return text != nullptr ? std::string {(const char*) text} : std::string {};
}

LinuxPattern linuxMatchFamily(const std::string& family)
{
    auto pattern = LinuxPattern {FcPatternCreate()};

    if (!pattern)
        return {};

    FcPatternAddString(pattern.get(), FC_FAMILY, (const FcChar8*) family.c_str());
    FcConfigSubstitute(nullptr, pattern.get(), FcMatchPattern);
    FcDefaultSubstitute(pattern.get());

    auto status = FcResult {};

    return LinuxPattern {FcFontMatch(nullptr, pattern.get(), &status)};
}

std::string linuxFamilyOf(FcPattern* pattern, const std::string& wanted)
{
    auto first = std::string {};

    for (auto index = 0;; ++index)
    {
        FcChar8* name = nullptr;

        if (FcPatternGetString(pattern, FC_FAMILY, index, &name) != FcResultMatch)
            break;

        const auto family = linuxToString(name);

        if (Strings::equalsCaseInsensitive(family, wanted))
            return family;

        if (first.empty())
            first = family;
    }

    return first;
}

LinuxFamilyFace linuxFaceOfPattern(FcPattern* pattern)
{
    auto face = LinuxFamilyFace {};
    FcChar8* file = nullptr;

    if (FcPatternGetString(pattern, FC_FILE, 0, &file) != FcResultMatch)
        return face;

    auto index = 0;

    if (FcPatternGetInteger(pattern, FC_INDEX, 0, &index) == FcResultMatch)
    {
        // The high half of a fontconfig index names a variable-font instance.
        if ((index >> 16) != 0)
            return {};

        face.faceIndex = index & 0xFFFF;
    }

    face.file = linuxToString(file);

    auto weight = 0;

    if (FcPatternGetInteger(pattern, FC_WEIGHT, 0, &weight) == FcResultMatch)
    {
        const auto opentype = FcWeightToOpenType(weight);

        face.weight = opentype > 0 ? weightClass(opentype) : 400;
    }
    else
    {
        // Not an integer means a range, which is what a variable font reports.
        face.weightVaries = true;
    }

    auto width = 0;

    if (FcPatternGetInteger(pattern, FC_WIDTH, 0, &width) == FcResultMatch)
        face.width = width;

    auto slant = 0;

    if (FcPatternGetInteger(pattern, FC_SLANT, 0, &slant) == FcResultMatch)
        face.italic = slant != FC_SLANT_ROMAN;

    return face;
}

Vector<LinuxFamilyFace> linuxFacesOfFamily(const std::string& family)
{
    auto faces = Vector<LinuxFamilyFace> {};
    auto pattern = LinuxPattern {FcPatternCreate()};
    auto objects = LinuxObjectSet {
        FcObjectSetBuild(FC_FILE, FC_INDEX, FC_WEIGHT, FC_WIDTH, FC_SLANT, nullptr)};

    if (!pattern || !objects)
        return faces;

    FcPatternAddString(pattern.get(), FC_FAMILY, (const FcChar8*) family.c_str());

    auto set = LinuxFontSet {FcFontList(nullptr, pattern.get(), objects.get())};

    if (!set)
        return faces;

    for (auto index = 0; index < set->nfont; ++index)
        if (auto face = linuxFaceOfPattern(set->fonts[index]); !face.isEmpty())
            faces.add(std::move(face));

    return faces;
}

std::string linuxFamilyOfPostScriptName(const std::string& name)
{
    auto pattern = LinuxPattern {FcPatternCreate()};
    auto objects = LinuxObjectSet {FcObjectSetBuild(FC_FAMILY, nullptr)};

    if (!pattern || !objects)
        return {};

    FcPatternAddString(
        pattern.get(), FC_POSTSCRIPT_NAME, (const FcChar8*) name.c_str());

    auto set = LinuxFontSet {FcFontList(nullptr, pattern.get(), objects.get())};

    if (!set || set->nfont <= 0)
        return {};

    return linuxFamilyOf(set->fonts[0], {});
}

LinuxFontSet linuxSortedFonts(const std::string& family, const FontVariant& variant)
{
    auto pattern = LinuxPattern {FcPatternCreate()};

    if (!pattern)
        return {};

    FcPatternAddString(pattern.get(), FC_FAMILY, (const FcChar8*) family.c_str());
    FcPatternAddInteger(
        pattern.get(), FC_WEIGHT, FcWeightFromOpenType(weightClass(variant.weight)));
    FcPatternAddInteger(
        pattern.get(), FC_SLANT, variant.italic ? FC_SLANT_ITALIC : FC_SLANT_ROMAN);

    FcConfigSubstitute(nullptr, pattern.get(), FcMatchPattern);
    FcDefaultSubstitute(pattern.get());

    auto status = FcResult {};

    return LinuxFontSet {
        FcFontSort(nullptr, pattern.get(), FcTrue, nullptr, &status)};
}

bool linuxPatternHasChar(FcPattern* pattern, char32_t codepoint)
{
    FcCharSet* charset = nullptr;

    if (FcPatternGetCharSet(pattern, FC_CHARSET, 0, &charset) != FcResultMatch)
        return false;

    return FcCharSetHasChar(charset, (FcChar32) codepoint) == FcTrue;
}

bool linuxPatternIsColor(FcPattern* pattern)
{
    auto color = FcBool {FcFalse};

    return FcPatternGetBool(pattern, FC_COLOR, 0, &color) == FcResultMatch
           && color == FcTrue;
}

struct LinuxSizedFace
{
    LinuxFace face;
    LinuxShaperFace shaperFace;
    LinuxShaperFont font;

    std::string file;
    int faceIndex = 0;
    const LinuxMemoryFont* memory = nullptr;

    float pixelSize = 0.f;
    bool syntheticBold = false;
    bool syntheticOblique = false;
    bool color = false;

    float bitmapScale = 1.f;

    bool isSameFile(const LinuxFamilyFace& other) const
    {
        return memory == other.memory && faceIndex == other.faceIndex
               && file == other.file;
    }
};

// Returns the ratio of the size wanted to the strike used, 1 for an outline.
float linuxSizeFace(FT_Face face, float pixelSize)
{
    if (FT_IS_SCALABLE(face))
    {
        const auto size = (FT_F26Dot6) std::lround(pixelSize * 64.0);

        // 72 dpi, so a size in points is a size in pixels; the caller scaled.
        FT_Set_Char_Size(face, size, size, 72, 72);

        return 1.f;
    }

    if (face->num_fixed_sizes <= 0)
        return 1.f;

    const auto ppemOf = [face](int index)
    { return face->available_sizes[index].y_ppem / 64.f; };

    auto best = 0;

    for (auto index = 1; index < face->num_fixed_sizes; ++index)
        if (std::abs(ppemOf(index) - pixelSize) < std::abs(ppemOf(best) - pixelSize))
            best = index;

    if (FT_Select_Size(face, best) != 0 || ppemOf(best) <= 0.f)
        return 1.f;

    return pixelSize / ppemOf(best);
}

struct LinuxAxisSupply
{
    bool weight = false;
    bool slant = false;
};

LinuxAxisSupply linuxApplyAxes(FT_Face face,
                               const FontVariant& variant,
                               bool faceIsItalic,
                               float pointSize)
{
    auto supply = LinuxAxisSupply {};
    FT_MM_Var* axes = nullptr;

    if (!FT_HAS_MULTIPLE_MASTERS(face) || FT_Get_MM_Var(face, &axes) != 0
        || axes == nullptr)
        return supply;

    const auto wantsSlant = variant.italic && !faceIsItalic;
    auto hasItalicAxis = false;

    for (auto index = FT_UInt {}; index < axes->num_axis; ++index)
        if (axes->axis[index].tag == linuxItalicAxis)
            hasItalicAxis = true;

    auto coordinates = Vector<FT_Fixed>((int) axes->num_axis);

    for (auto index = FT_UInt {}; index < axes->num_axis; ++index)
    {
        const auto& axis = axes->axis[index];

        const auto clamped = [&axis](double amount)
        {
            return std::clamp((FT_Fixed) std::llround(amount * 65536.0),
                              axis.minimum,
                              axis.maximum);
        };

        auto value = axis.def;

        if (axis.tag == linuxWeightAxis)
        {
            value = clamped(weightClass(variant.weight));
            supply.weight = true;
        }
        else if (axis.tag == linuxItalicAxis && wantsSlant)
        {
            value = clamped(1.0);
            supply.slant = true;
        }
        else if (axis.tag == linuxSlantAxis && wantsSlant && !hasItalicAxis)
        {
            value = clamped(linuxObliqueSlant);
            supply.slant = true;
        }
        else if (axis.tag == linuxOpticalSizeAxis)
        {
            // Point size, never pixel size: the design must not vary with it.
            value = clamped(pointSize);
        }

        coordinates[(int) index] = value;
    }

    FT_Set_Var_Design_Coordinates(face, axes->num_axis, coordinates.data());
    FT_Done_MM_Var(fontSystem().library, axes);

    return supply;
}

bool linuxHasWeightAxis(FT_Face face)
{
    FT_MM_Var* axes = nullptr;

    if (!FT_HAS_MULTIPLE_MASTERS(face) || FT_Get_MM_Var(face, &axes) != 0
        || axes == nullptr)
        return false;

    auto found = false;

    for (auto index = FT_UInt {}; index < axes->num_axis; ++index)
        if (axes->axis[index].tag == linuxWeightAxis)
            found = true;

    FT_Done_MM_Var(fontSystem().library, axes);

    return found;
}

// hb-ft would read a bitmap strike's own size (109px for Noto Color Emoji) as
// the em, so a non-scalable face gets HarfBuzz's OpenType metrics instead.
LinuxShaperFont linuxMakeShaperFont(LinuxSizedFace& sized)
{
    auto* face = sized.face.get();

    if (FT_IS_SCALABLE(face))
    {
        auto font = LinuxShaperFont {hb_ft_font_create_referenced(face)};

        if (font)
            hb_ft_font_set_load_flags(font.get(), FT_LOAD_NO_HINTING);

        return font;
    }

    sized.shaperFace.reset(hb_ft_face_create_referenced(face));

    if (!sized.shaperFace)
        return {};

    auto font = LinuxShaperFont {hb_font_create(sized.shaperFace.get())};
    const auto scale = (int) std::lround(sized.pixelSize * 64.0);

    if (font)
        hb_font_set_scale(font.get(), scale, scale);

    return font;
}

void linuxAttachShaper(LinuxSizedFace& sized)
{
    sized.font = linuxMakeShaperFont(sized);

    if (!sized.font)
        return;

#if HB_VERSION_ATLEAST(7, 0, 0)
    if (sized.syntheticBold)
        hb_font_set_synthetic_bold(sized.font.get(),
                                   linuxSyntheticBoldRatio,
                                   linuxSyntheticBoldRatio,
                                   false);
#endif

#if HB_VERSION_ATLEAST(3, 3, 0)
    if (sized.syntheticOblique)
        hb_font_set_synthetic_slant(sized.font.get(), linuxObliqueShear);
#endif
}

bool linuxOpenFace(LinuxSizedFace& sized,
                   const LinuxFamilyFace& source,
                   float pixelSize)
{
    auto& system = fontSystem();
    FT_Face raw = nullptr;

    const auto opened =
        source.memory != nullptr
            ? FT_New_Memory_Face(system.library,
                                 source.memory->bytes.data(),
                                 (FT_Long) source.memory->bytes.size(),
                                 source.faceIndex,
                                 &raw)
            : FT_New_Face(
                  system.library, source.file.c_str(), source.faceIndex, &raw);

    if (opened != 0 || raw == nullptr)
        return false;

    sized.face.reset(raw);
    sized.file = source.file;
    sized.faceIndex = source.faceIndex;
    sized.memory = source.memory;
    sized.pixelSize = pixelSize;
    sized.color = FT_HAS_COLOR(raw);
    sized.bitmapScale = linuxSizeFace(raw, pixelSize);

    return true;
}

bool linuxIsRealScript(hb_script_t script)
{
    return script != HB_SCRIPT_COMMON && script != HB_SCRIPT_INHERITED
           && script != HB_SCRIPT_UNKNOWN && script != HB_SCRIPT_INVALID;
}

// Not the full Unicode property: the plane-1 default-emoji range plus VS16.
bool linuxWantsColorFont(char32_t codepoint, char32_t next)
{
    return next == 0xFE0F || (codepoint >= 0x1F000 && codepoint <= 0x1FAFF);
}

struct LinuxTextPoint
{
    char32_t value = 0;
    int begin = 0;
    int end = 0;
    hb_script_t script = HB_SCRIPT_COMMON;
    int font = 0;
};

struct LinuxTextItem
{
    int begin = 0;
    int end = 0;
    hb_script_t script = HB_SCRIPT_COMMON;
    int font = 0;
};

// Common, Inherited and Unknown are not scripts a shaper can use.
void linuxResolveScripts(Vector<LinuxTextPoint>& points)
{
    auto carried = HB_SCRIPT_COMMON;

    for (auto& point: points)
    {
        if (linuxIsRealScript(point.script))
            carried = point.script;
        else
            point.script = carried;
    }

    auto following = HB_SCRIPT_COMMON;

    for (auto index = points.size() - 1; index >= 0; --index)
    {
        if (linuxIsRealScript(points[index].script))
            following = points[index].script;
        else
            points[index].script = following;
    }
}

hb_direction_t linuxDirectionOf(hb_script_t script)
{
    const auto direction = hb_script_get_horizontal_direction(script);

    return direction == HB_DIRECTION_INVALID ? HB_DIRECTION_LTR : direction;
}
} // namespace LinuxText

using namespace LinuxText;

struct GlyphRasterizer::Native
{
    explicit Native(const FontRequest& requestToUse)
        : request(requestToUse)
    {
        auto& system = fontSystem();
        const auto lock = std::lock_guard {system.mutex};

        if (!system.ready)
            return;

        resolve();

        if (familyFaces.empty())
            return;

        valid = variantFace({}) != nullptr;
    }

    ~Native()
    {
        const auto lock = std::lock_guard {fontSystem().mutex};

        variants.clear();
        fallbacks.clear();
        sortedFonts.clear();
    }

    void resolve()
    {
        if (resolveRegistered())
            return;

        auto matched = linuxMatchFamily(request.family);

        if (!matched)
            return;

        resolved = linuxFamilyOf(matched.get(), request.family);

        if (!Strings::equalsCaseInsensitive(resolved, request.family))
            if (auto named = linuxFamilyOfPostScriptName(request.family);
                !named.empty())
                resolved = named;

        if (resolved.empty())
            return;

        familyFaces = linuxFacesOfFamily(resolved);

        // A family fontconfig only knows as a substitution lists no faces.
        if (familyFaces.empty())
            if (auto face = linuxFaceOfPattern(matched.get()); !face.isEmpty())
                familyFaces.add(std::move(face));
    }

    bool resolveRegistered()
    {
        for (const auto& font: fontSystem().memoryFonts)
            if (Strings::equalsCaseInsensitive(font->family, request.family)
                || Strings::equalsCaseInsensitive(font->postScriptName,
                                                  request.family))
            {
                resolved = font->family;
                break;
            }

        if (resolved.empty())
            return false;

        for (const auto& font: fontSystem().memoryFonts)
            if (Strings::equalsCaseInsensitive(font->family, resolved))
                familyFaces.add(LinuxFamilyFace {{},
                                                 0,
                                                 font.get(),
                                                 font->weight,
                                                 linuxNormalWidth,
                                                 font->italic,
                                                 font->weightVaries});

        return true;
    }

    static int variantKey(const FontVariant& variant)
    {
        return weightClass(variant.weight) * 2 + (variant.italic ? 1 : 0);
    }

    const LinuxSizedFace* variantFace(const FontVariant& variant) const
    {
        const auto key = variantKey(variant);

        if (auto found = variants.find(key); found != variants.end())
            return found->second.font ? &found->second : nullptr;

        auto& entry = variants.emplace(key, makeVariantFace(variant)).first->second;

        return entry.font ? &entry : nullptr;
    }

    LinuxSizedFace makeVariantFace(const FontVariant& variant) const
    {
        auto sized = LinuxSizedFace {};
        const auto* source = linuxNearestFace(familyFaces, variant);

        if (source == nullptr || !linuxOpenFace(sized, *source, request.pixelSize()))
            return {};

        const auto supplied = linuxApplyAxes(
            sized.face.get(), variant, source->italic, request.pointSize);

        sized.syntheticBold = weightClass(variant.weight) >= 600
                              && source->weight < 600 && !supplied.weight;
        sized.syntheticOblique =
            variant.italic && !source->italic && !supplied.slant;

        linuxAttachShaper(sized);

        return sized;
    }

    const LinuxSizedFace* fontAt(int font, const FontVariant& variant) const
    {
        if (font == 0)
            return variantFace(variant);

        const auto index = font - 1;

        if (index < 0 || index >= fallbacks.size())
            return nullptr;

        return fallbacks[index].get();
    }

    FontMetrics metrics(const FontVariant& variant) const
    {
        const auto lock = std::lock_guard {fontSystem().mutex};

        auto result = FontMetrics {};
        const auto* sized = variantFace(variant);

        if (sized == nullptr)
            return result;

        auto* face = sized->face.get();
        const auto& sizeMetrics = face->size->metrics;

        if (FT_IS_SCALABLE(face))
        {
            // Design metrics, not the grid-fitted ones, so line height is
            // stable across fractional sizes.
            const auto scaled = [&sizeMetrics](FT_Short units)
            { return FT_MulFix(units, sizeMetrics.y_scale) / 64.f; };

            result.ascent = scaled(face->ascender);
            result.descent = -scaled(face->descender);
            result.leading = scaled(face->height) - result.ascent - result.descent;
        }
        else
        {
            const auto strike = sized->bitmapScale;

            result.ascent = sizeMetrics.ascender / 64.f * strike;
            result.descent = -sizeMetrics.descender / 64.f * strike;
            result.leading =
                sizeMetrics.height / 64.f * strike - result.ascent - result.descent;
        }

        // Some faces report a line shorter than ascent plus descent.
        result.leading = std::max(0.f, result.leading);

        result.advance = advanceOf(*sized, FT_Get_Char_Index(face, 'M'));

        return result;
    }

    static float advanceOf(const LinuxSizedFace& sized, std::uint32_t glyph)
    {
        if (!sized.font)
            return 0.f;

        return hb_font_get_glyph_h_advance(sized.font.get(), (hb_codepoint_t) glyph)
               / 64.f;
    }

    ShapedRun shape(std::string_view text, const FontVariant& variant) const
    {
        const auto lock = std::lock_guard {fontSystem().mutex};

        auto result = ShapedRun {};

        if (text.empty() || variantFace(variant) == nullptr)
            return result;

        auto pen = 0.f;

        for (const auto& item: itemize(text, variant))
            shapeItem(text, item, variant, pen, result);

        result.advance = pen;

        return result;
    }

    // No bidi: runs stay in logical order, each shaped in its own direction.
    Vector<LinuxTextItem> itemize(std::string_view text,
                                  const FontVariant& variant) const
    {
        auto points = Vector<LinuxTextPoint> {};
        auto index = 0;

        while (index < (int) text.size())
        {
            const auto begin = index;
            const auto value = decodeUtf8(text, index);

            points.add({value, begin, index});
        }

        auto* unicode = hb_unicode_funcs_get_default();

        for (auto& point: points)
            point.script = hb_unicode_script(unicode, (hb_codepoint_t) point.value);

        linuxResolveScripts(points);

        for (auto at = 0; at < points.size(); ++at)
            points[at].font =
                fontFor(points[at].value,
                        at + 1 < points.size() ? points[at + 1].value : char32_t {},
                        variant);

        auto items = Vector<LinuxTextItem> {};

        for (const auto& point: points)
        {
            const auto last = items.size() - 1;

            if (!items.empty() && items[last].script == point.script
                && items[last].font == point.font)
                items[last].end = point.end;
            else
                items.add({point.begin, point.end, point.script, point.font});
        }

        return items;
    }

    int fontFor(char32_t codepoint, char32_t next, const FontVariant& variant) const
    {
        const auto* base = variantFace(variant);

        if (base == nullptr)
            return 0;

        const auto wantsColor = linuxWantsColorFont(codepoint, next);
        const auto baseHas =
            FT_Get_Char_Index(base->face.get(), (FT_ULong) codepoint) != 0;

        if (baseHas && (base->color || !wantsColor))
            return 0;

        const auto key =
            ((std::uint64_t) variantKey(variant) * 2 + (wantsColor ? 1 : 0)) << 32
            | codepoint;

        if (const auto found = fallbackChoices.find(key);
            found != fallbackChoices.end())
            return found->second;

        const auto font = fallbackFor(codepoint, wantsColor, variant);

        fallbackChoices.emplace(key, font);

        return font;
    }

    int fallbackFor(char32_t codepoint,
                    bool wantsColor,
                    const FontVariant& variant) const
    {
        auto* set = sortedSetFor(variant);

        if (set == nullptr)
            return 0;

        // Colour pass first when asked, or the sort offers a mono face sooner.
        for (auto pass = wantsColor ? 0 : 1; pass < 2; ++pass)
            for (auto index = 0; index < set->nfont; ++index)
            {
                auto* pattern = set->fonts[index];

                if (pass == 0 && !linuxPatternIsColor(pattern))
                    continue;

                if (!linuxPatternHasChar(pattern, codepoint))
                    continue;

                if (const auto font = adoptFallback(pattern, variant); font > 0)
                    return font;
            }

        return 0;
    }

    // Its place in the table plus one; zero is the face the request asked for.
    int adoptFallback(FcPattern* pattern, const FontVariant& variant) const
    {
        const auto source = linuxFaceOfPattern(pattern);

        if (source.isEmpty())
            return 0;

        if (const auto* base = variantFace(variant);
            base != nullptr && base->isSameFile(source))
            return 0;

        for (auto index = 0; index < fallbacks.size(); ++index)
            if (fallbacks[index]->isSameFile(source))
                return index + 1;

        // GlyphAtlas keys a glyph on eight bits of font number.
        if (fallbacks.size() >= 255)
            return 0;

        auto sized = std::make_unique<LinuxSizedFace>();

        if (!linuxOpenFace(*sized, source, request.pixelSize()))
            return 0;

        linuxAttachShaper(*sized);

        if (!sized->font)
            return 0;

        fallbacks.add(std::move(sized));

        return fallbacks.size();
    }

    FcFontSet* sortedSetFor(const FontVariant& variant) const
    {
        const auto key = variantKey(variant);
        auto found = sortedFonts.find(key);

        if (found == sortedFonts.end())
            found =
                sortedFonts.emplace(key, linuxSortedFonts(resolved, variant)).first;

        return found->second.get();
    }

    void shapeItem(std::string_view text,
                   const LinuxTextItem& item,
                   const FontVariant& variant,
                   float& pen,
                   ShapedRun& result) const
    {
        const auto* sized = fontAt(item.font, variant);

        if (sized == nullptr)
            return;

        auto buffer = LinuxShaperBuffer {hb_buffer_create()};

        if (!buffer)
            return;

        // The whole string with the item as a range, so clusters are byte
        // offsets into the caller's text and the shaper sees the context.
        hb_buffer_set_cluster_level(buffer.get(),
                                    HB_BUFFER_CLUSTER_LEVEL_MONOTONE_CHARACTERS);
        hb_buffer_add_utf8(buffer.get(),
                           text.data(),
                           (int) text.size(),
                           (unsigned int) item.begin,
                           item.end - item.begin);

        hb_buffer_set_script(buffer.get(), item.script);
        hb_buffer_set_direction(buffer.get(), linuxDirectionOf(item.script));
        hb_buffer_set_language(buffer.get(), hb_language_from_string("en", -1));

        hb_shape(sized->font.get(), buffer.get(), nullptr, 0);

        auto count = 0u;
        const auto* infos = hb_buffer_get_glyph_infos(buffer.get(), &count);
        const auto* positions = hb_buffer_get_glyph_positions(buffer.get(), &count);

        if (infos == nullptr || positions == nullptr)
            return;

        for (auto index = 0u; index < count; ++index)
        {
            result.glyphs.add({{infos[index].codepoint, item.font},
                               pen + positions[index].x_offset / 64.f,
                               positions[index].y_offset / 64.f,
                               (int) infos[index].cluster});

            pen += positions[index].x_advance / 64.f;
        }
    }

    GlyphBitmap rasterize(GlyphKey key,
                          const FontVariant& variant,
                          const RasterRequest& raster) const
    {
        const auto lock = std::lock_guard {fontSystem().mutex};

        auto result = GlyphBitmap {};
        const auto* sized = fontAt(key.font, variant);

        if (sized == nullptr)
            return result;

        result.valid = true;
        result.advance = advanceOf(*sized, key.glyph);

        drawGlyph(*sized, key.glyph, raster, result);

        return result;
    }

    // FreeType has no light-text thickening; both RasterRequest masks are one.
    static void drawGlyph(const LinuxSizedFace& sized,
                          std::uint32_t glyph,
                          const RasterRequest& raster,
                          GlyphBitmap& bitmap)
    {
        auto* face = sized.face.get();

        // Vertical-only hinting, so a stem keeps its subpixel position. Never
        // LCD: it would bake one text colour into the coverage cached here.
        auto flags = FT_Int32 {FT_LOAD_TARGET_LIGHT};

        flags |= sized.color ? FT_LOAD_COLOR : FT_LOAD_NO_BITMAP;

        if (FT_Load_Glyph(face, glyph, flags) != 0)
            return;

        auto* slot = face->glyph;

        if (slot->format == FT_GLYPH_FORMAT_OUTLINE)
            adjustOutline(sized, raster, slot->outline);

        if (slot->format != FT_GLYPH_FORMAT_BITMAP
            && FT_Render_Glyph(slot, FT_RENDER_MODE_NORMAL) != 0)
            return;

        // Compared as the enum, not the byte: a unity build leaks `using
        // namespace eacp::GPU` in, whose EDSL operator== Clang would pick.
        const auto pixelMode = (FT_Pixel_Mode) slot->bitmap.pixel_mode;

        if (pixelMode == FT_PIXEL_MODE_BGRA)
            takeColorBitmap(sized, *slot, bitmap);
        else if (pixelMode == FT_PIXEL_MODE_GRAY)
            takeMaskBitmap(*slot, bitmap);
        else
            takeEmptyBitmap(*slot, bitmap);
    }

    static void adjustOutline(const LinuxSizedFace& sized,
                              const RasterRequest& raster,
                              FT_Outline& outline)
    {
        if (sized.syntheticOblique)
        {
            auto shear =
                FT_Matrix {0x10000,
                           (FT_Fixed) std::lround(linuxObliqueShear * 65536.0),
                           0,
                           0x10000};

            FT_Outline_Transform(&outline, &shear);
        }

        if (sized.syntheticBold)
        {
            const auto strength = (FT_Pos) std::lround(linuxSyntheticBoldRatio
                                                       * sized.pixelSize * 64.0);

            FT_Outline_EmboldenXY(&outline, strength, 0);
        }

        const auto shift = std::clamp(raster.subpixelX, 0.f, 1.f);

        FT_Outline_Translate(&outline, (FT_Pos) std::lround(shift * 64.0), 0);
    }

    static void takeEmptyBitmap(const FT_GlyphSlotRec& slot, GlyphBitmap& bitmap)
    {
        bitmap.bearingX = (float) slot.bitmap_left;
        bitmap.bearingY = (float) slot.bitmap_top;
    }

    static void takeMaskBitmap(const FT_GlyphSlotRec& slot, GlyphBitmap& bitmap)
    {
        const auto& source = slot.bitmap;

        takeEmptyBitmap(slot, bitmap);

        // A glyph with nothing to draw is a space: valid, and it advances.
        if (source.width == 0 || source.rows == 0 || source.buffer == nullptr)
            return;

        bitmap.width = (int) source.width;
        bitmap.height = (int) source.rows;
        bitmap.pixels.assign(bitmap.width * bitmap.height, std::uint8_t {0});

        for (auto y = 0; y < bitmap.height; ++y)
        {
            // The pitch is negative for a bitmap whose rows run upwards.
            const auto* row = source.buffer + (std::ptrdiff_t) y * source.pitch;

            std::copy(row,
                      row + bitmap.width,
                      bitmap.pixels.data() + (std::size_t) y * bitmap.width);
        }
    }

    // FreeType's premultiplied BGRA to the straight RGBA the atlas stores.
    static void takeColorBitmap(const LinuxSizedFace& sized,
                                const FT_GlyphSlotRec& slot,
                                GlyphBitmap& bitmap)
    {
        const auto& source = slot.bitmap;
        const auto scale = sized.bitmapScale;

        bitmap.format = GlyphFormat::Color;
        bitmap.bearingX = (float) slot.bitmap_left * scale;
        bitmap.bearingY = (float) slot.bitmap_top * scale;

        if (source.width == 0 || source.rows == 0 || source.buffer == nullptr)
            return;

        bitmap.width = std::max(1, (int) std::lround(source.width * scale));
        bitmap.height = std::max(1, (int) std::lround(source.rows * scale));
        bitmap.pixels.assign(bitmap.width * bitmap.height * 4, std::uint8_t {0});

        for (auto y = 0; y < bitmap.height; ++y)
            for (auto x = 0; x < bitmap.width; ++x)
                sampleColorPixel(source, bitmap, x, y);
    }

    // Averaged premultiplied, where compositing is linear, then written out.
    static void
        sampleColorPixel(const FT_Bitmap& source, GlyphBitmap& bitmap, int x, int y)
    {
        const auto span = [](int at, int count, int of)
        {
            const auto begin = (int) ((std::int64_t) at * of / count);
            const auto end = (int) ((std::int64_t) (at + 1) * of / count);

            return std::pair {begin, std::max(begin + 1, end)};
        };

        const auto [left, right] = span(x, bitmap.width, (int) source.width);
        const auto [top, bottom] = span(y, bitmap.height, (int) source.rows);

        std::int64_t totals[4] = {};
        auto count = 0;

        for (auto row = top; row < bottom && row < (int) source.rows; ++row)
            for (auto column = left; column < right && column < (int) source.width;
                 ++column)
            {
                const auto* pixel = source.buffer
                                    + (std::ptrdiff_t) row * source.pitch
                                    + (std::ptrdiff_t) column * 4;

                // FreeType lays a colour pixel out blue first.
                totals[0] += pixel[2];
                totals[1] += pixel[1];
                totals[2] += pixel[0];
                totals[3] += pixel[3];
                ++count;
            }

        if (count == 0)
            return;

        auto* target =
            bitmap.pixels.data() + ((std::size_t) y * bitmap.width + x) * 4;
        const auto alpha = (int) (totals[3] / count);

        target[3] = (std::uint8_t) alpha;

        for (auto channel = 0; channel < 3; ++channel)
        {
            const auto value = (int) (totals[channel] / count);

            target[channel] =
                (std::uint8_t) (alpha > 0 ? std::min(value * 255 / alpha, 255) : 0);
        }
    }

    FontRequest request;
    std::string resolved;
    Vector<LinuxFamilyFace> familyFaces;
    mutable std::map<int, LinuxSizedFace> variants;
    mutable std::map<int, LinuxFontSet> sortedFonts;
    mutable Vector<std::unique_ptr<LinuxSizedFace>> fallbacks;
    mutable std::map<std::uint64_t, int> fallbackChoices;
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

ShapedRun GlyphRasterizer::shape(std::string_view text,
                                 const FontVariant& variant) const
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
    const auto run = impl->shape({encoded, (std::size_t) length}, variant);

    if (run.glyphs.empty())
        return {};

    return impl->rasterize(run.glyphs[0].key, variant, {});
}

const FontRequest& GlyphRasterizer::request() const
{
    return impl->request;
}

std::optional<RegisteredFont> registerMemoryFont(const void* data, int size)
{
    if (data == nullptr || size <= 0)
        return std::nullopt;

    auto& system = fontSystem();
    const auto lock = std::lock_guard {system.mutex};

    if (!system.ready)
        return std::nullopt;

    auto font = std::make_unique<LinuxMemoryFont>();
    const auto* bytes = (const std::uint8_t*) data;

    font->bytes.assign(bytes, bytes + size);

    FT_Face raw = nullptr;

    if (FT_New_Memory_Face(
            system.library, font->bytes.data(), (FT_Long) size, 0, &raw)
            != 0
        || raw == nullptr)
        return std::nullopt;

    auto face = LinuxFace {raw};

    if (face->family_name != nullptr)
        font->family = face->family_name;

    if (const auto* postScript = FT_Get_Postscript_Name(face.get()))
        font->postScriptName = postScript;

    if (font->family.empty() || font->postScriptName.empty())
        return std::nullopt;

    font->italic = (face->style_flags & FT_STYLE_FLAG_ITALIC) != 0;
    font->weightVaries = linuxHasWeightAxis(face.get());

    if (const auto* os2 = (const TT_OS2*) FT_Get_Sfnt_Table(face.get(), FT_SFNT_OS2);
        os2 != nullptr && os2->version != 0xFFFF && os2->usWeightClass > 0)
        font->weight = weightClass((int) os2->usWeightClass);

    for (const auto& existing: system.memoryFonts)
        if (Strings::equalsCaseInsensitive(existing->postScriptName,
                                           font->postScriptName))
            return RegisteredFont {existing->family, existing->postScriptName};

    auto names = RegisteredFont {font->family, font->postScriptName};

    system.memoryFonts.add(std::move(font));

    return names;
}
} // namespace eacp::Text

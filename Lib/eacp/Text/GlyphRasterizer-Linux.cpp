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

// FreeType, HarfBuzz and fontconfig, the Linux counterpart to
// GlyphRasterizer-Apple.mm and GlyphRasterizer-Windows.cpp. The three of them
// are the parts CoreText and DirectWrite each roll into one: fontconfig
// answers "which file is that family" and "which file has this codepoint",
// HarfBuzz kerns, ligates and places marks, FreeType turns an outline into
// coverage.
//
// The one thing neither library does is decide what to shape with. CoreText
// and DirectWrite itemize a string into runs of one script and one face before
// they shape it; HarfBuzz shapes a run it is handed and expects the caller to
// have split it. So the itemizer here is the piece with no counterpart in the
// other two files: it walks the codepoints, gives every one a script (with
// Common, Inherited and Unknown joining the neighbouring real script, so a
// mark stays with its base and a space does not split a word) and a font (the
// base face when its cmap has the codepoint, else the first face fontconfig's
// sort offers that does), and hands HarfBuzz one run per change of either.
//
// Not done here, and a deliberate follow-up: bidi. A line of mixed direction
// is shaped run by run in logical order, each run in its script's own
// direction, with no reordering between them - so Arabic inside English draws
// its own glyphs the right way round but in the wrong place on the line.
// Doing better means a bidi pass (fribidi, or Unicode UBA by hand) above this
// seam rather than inside it, since the reordering is a property of the
// paragraph and not of the face.

namespace eacp::Text
{
// A named namespace rather than an anonymous one, because GlyphRasterizer's
// Native holds several of these types as members and Native is declared in a
// header. Under a unity build its definition arrives through an #include,
// which is enough for GCC to read a member of internal-linkage type as an ODR
// hazard and warn (-Wsubobject-linkage). The name is what keeps these out of
// the way of the other translation units the unity build concatenates.
namespace LinuxText
{
// ---------------------------------------------------------------------------
// Handles. Every FreeType, HarfBuzz and fontconfig object below is owned by
// one of these, so nothing in this file calls a Done/destroy function by hand.

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

// ---------------------------------------------------------------------------
// Process-wide state.

// A face registered from memory. The bytes are kept for the life of the
// process because every FT_Face opened from them reads out of them lazily, and
// a page that registered a web font can outlive any one rasterizer.
struct LinuxMemoryFont
{
    Vector<std::uint8_t> bytes;
    std::string family;
    std::string postScriptName;
    int weight = 400;
    bool italic = false;
    bool weightVaries = false;
};

// One FT_Library and one fontconfig for the process, behind one mutex.
//
// FreeType's library and its faces are not thread safe - two threads inside
// the same FT_Library corrupt its internal allocator - and HarfBuzz's ft funcs
// reach straight into the FT_Face they were built from. So every call into any
// of the three is made under this lock. A finer scheme is possible (a lock per
// face, a library per thread) and is not worth the failure mode it risks.
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
    // Deliberately never torn down. Both halves of it outlive any orderly
    // shutdown: FT_Done_FreeType would run at exit before the faces of a
    // rasterizer some other static still holds, and the registered bytes are
    // read lazily by every one of those faces.
    static auto* instance = new LinuxFontSystem {};

    return *instance;
}

// ---------------------------------------------------------------------------
// Matching.

constexpr FT_ULong linuxAxisTag(char a, char b, char c, char d)
{
    return ((FT_ULong) a << 24) | ((FT_ULong) b << 16) | ((FT_ULong) c << 8)
           | (FT_ULong) d;
}

constexpr auto linuxWeightAxis = linuxAxisTag('w', 'g', 'h', 't');
constexpr auto linuxItalicAxis = linuxAxisTag('i', 't', 'a', 'l');
constexpr auto linuxSlantAxis = linuxAxisTag('s', 'l', 'n', 't');
constexpr auto linuxOpticalSizeAxis = linuxAxisTag('o', 'p', 's', 'z');

// What CSS's oblique is on the slnt axis, which counts degrees anticlockwise
// from upright and so runs negative for the way a Latin italic leans.
constexpr float linuxObliqueSlant = -14.f;

// The same lean as a shear, tan(14°): what a synthesized oblique applies when
// the face has no axis to ask.
constexpr float linuxObliqueShear = 0.24933f;

// How much of the em a synthesized bold adds to a stem. HarfBuzz takes it as a
// fraction of the em and FreeType as a distance in pixels, so one number here
// keeps what is shaped and what is drawn in step.
constexpr float linuxSyntheticBoldRatio = 0.02f;

// fontconfig's normal width. Faces are matched by how far they are from it,
// which is CSS's stretch axis and is matched before slant and weight.
constexpr int linuxNormalWidth = 100;

// How far a face's weight is from the one wanted, by CSS Fonts' matching:
// from 400 the search goes to 500 first, then downwards, then up; from 500 to
// 400 first, then down, then up; below 400 downwards then up; above 500
// upwards then down. Smaller is nearer. The same rule as
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

// One face of a family, as fontconfig or the memory registry described it -
// enough to match against, and enough to open.
struct LinuxFamilyFace
{
    std::string file;
    int faceIndex = 0;
    const LinuxMemoryFont* memory = nullptr;
    int weight = 400;
    int width = linuxNormalWidth;
    bool italic = false;

    // A variable face's weight is a range rather than a number, and its axis
    // supplies whatever is asked of it, so it is never the wrong weight.
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

// ---------------------------------------------------------------------------
// fontconfig.

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

// The family a matched pattern says it is: the name asked for when the face
// carries it (a face is often filed under several - DejaVu's ExtraLight is
// both "DejaVu Sans" and "DejaVu Sans Light"), else the first one it has.
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
        // The high half of the index names the instance of a variable font
        // fontconfig is describing. Only the master is worth opening: its axes
        // reach every instance, and the instances would otherwise crowd the
        // family with faces that are all the same file.
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

// The family holding a face of that PostScript name, empty when none does.
// A FontRequest's family may be either, since a caller with a web font knows
// the face by the name the file names itself.
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

// Every face on the machine, best first, for the family and variant asked
// for: what a codepoint the family lacks is looked for in.
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

// ---------------------------------------------------------------------------
// Faces.

// A face opened at one size, with the shaper's view of it beside it: one per
// (weight class, slant) of the family, and one per fallback face met.
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

    // A colour bitmap strike is one fixed size, so everything FreeType reports
    // about a glyph from it has to be scaled to the size actually asked for.
    float bitmapScale = 1.f;

    bool isSameFile(const LinuxFamilyFace& other) const
    {
        return memory == other.memory && faceIndex == other.faceIndex
               && file == other.file;
    }
};

// Sizes a face and reports how far its bitmaps are from the size wanted: 1 for
// an outline face, which takes any size, and the ratio for a bitmap-only face,
// which takes only the strikes it was built with.
float linuxSizeFace(FT_Face face, float pixelSize)
{
    if (FT_IS_SCALABLE(face))
    {
        const auto size = (FT_F26Dot6) std::lround(pixelSize * 64.0);

        // 72 dpi, so a size in points is a size in pixels: the caller already
        // multiplied its point size by the device scale.
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

// The axes a variant asked of a face, so the caller knows what it no longer
// has to synthesize.
struct LinuxAxisSupply
{
    bool weight = false;
    bool slant = false;
};

// Moves a variable face along its own axes to the variant asked for. A family
// cut into one file per weight has no axes and answers nothing here; a
// variable face is every weight in its range in one file, which is the one
// case matching a sibling face cannot answer.
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
            // Pinned to the point size, never the pixel size. The scale is how
            // finely a glyph is rasterized and nothing else, so a face that
            // varies by optical size must be shaped in the same design on a
            // Retina panel and off one - see the same pin in
            // GlyphRasterizer-Apple.mm's withPinnedAxes.
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

// The shaper's view of an already sized face.
//
// An outline face goes through hb-ft, so HarfBuzz measures with the same
// FreeType the glyphs are drawn from and the two can never disagree; the load
// flags say unhinted, which is the linear advance the atlas lays out in.
//
// A bitmap-only face cannot: hb-ft reads its metrics off the FT face, which
// for a strike is the strike's own size - 109 pixels for Noto Color Emoji -
// and no scale set on the HarfBuzz side changes that. HarfBuzz's own OpenType
// metrics read hmtx against the em instead, so the same face advances the pen
// correctly at 24.
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

// ---------------------------------------------------------------------------
// Itemization.

bool linuxIsRealScript(hb_script_t script)
{
    return script != HB_SCRIPT_COMMON && script != HB_SCRIPT_INHERITED
           && script != HB_SCRIPT_UNKNOWN && script != HB_SCRIPT_INVALID;
}

// Whether a codepoint asks to be drawn as an emoji rather than as a letter.
// Not the full Unicode property, which is a table of its own, but the plane-1
// range where every assigned character has emoji presentation by default, plus
// the explicit request an emoji variation selector makes. That is enough for
// the case that matters: a family whose fallback chain offers both a colour
// face and a monochrome outline for the same codepoint - DejaVu Sans carries
// U+1F600 as a line drawing - must pick the colour one.
bool linuxWantsColorFont(char32_t codepoint, char32_t next)
{
    return next == 0xFE0F || (codepoint >= 0x1F000 && codepoint <= 0x1FAFF);
}

// One codepoint of the text, with what shaping it needs decided.
struct LinuxTextPoint
{
    char32_t value = 0;
    int begin = 0;
    int end = 0;
    hb_script_t script = HB_SCRIPT_COMMON;
    int font = 0;
};

// A maximal stretch of one script in one font: what HarfBuzz is handed.
struct LinuxTextItem
{
    int begin = 0;
    int end = 0;
    hb_script_t script = HB_SCRIPT_COMMON;
    int font = 0;
};

// Common, Inherited and Unknown are not scripts a shaper can use: a space, a
// combining mark, a variation selector or a piece of punctuation belongs to
// whatever is around it. Carrying the last real script forwards, and the first
// one backwards over the start of the line, keeps a mark with its base and
// stops punctuation splitting a word into two runs.
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

    // The registry first, so a face registered from memory resolves without
    // ever reaching the machine's font directories, then fontconfig - which
    // answers with something for any name at all, and so is also where the
    // substitute for a family this machine does not have comes from.
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

        // A family fontconfig only knows as a substitution lists no faces of
        // its own; the pattern it matched is then the one face there is.
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

    // The nine CSS weights and the two slants, which is what a face is
    // cached, sorted and matched by: everything else about a variant is
    // supplied by the face rather than chosen between faces.
    static int variantKey(const FontVariant& variant)
    {
        return weightClass(variant.weight) * 2 + (variant.italic ? 1 : 0);
    }

    // The family's face nearest the variant, opened at the request's pixel
    // size and moved along whatever axes it has, built on first ask.
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

        // Only ever what neither a sibling face nor an axis could supply.
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
            // The unhinted design metrics rather than the grid-fitted ones the
            // size reports, so a line steps by the same height at every
            // fractional size - which is what CoreText and DirectWrite report.
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

        // A few faces describe a line shorter than their own ascent plus
        // descent; the atlas adds leading straight into line height, so a
        // negative one would overlap lines rather than tighten them.
        result.leading = std::max(0.f, result.leading);

        // 'M' is the conventional width probe; on a monospace face every glyph
        // shares this advance, and on a proportional one it is only a hint.
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

    // Which face draws a codepoint: the one asked for when its cmap has it,
    // else the first fontconfig offers that does, else the requested face
    // again - whose .notdef is a visible box and a better answer than a gap.
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

        // Remembered, because answering it means asking every font on the
        // machine whether its charset has the codepoint, and a line of CJK
        // asks the same question of every character in it.
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

        // Colour first when the codepoint asked for it, so an emoji lands in
        // Noto Color Emoji rather than in whatever monochrome face the sort
        // happened to offer sooner.
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

    // The number a fallback face shapes under: its place in the table, added
    // the first time it is met, so the same face always gets the same number.
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

        // The whole string goes in with the item as a range, so a cluster is a
        // byte offset into the text the caller gave and the shaper still sees
        // the context on either side of the run.
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

    // FreeType thickens nothing for light text - there is no counterpart to
    // CoreGraphics' font smoothing reading the fill's lightness - so the two
    // masks a RasterRequest can ask for are the same mask.
    static void drawGlyph(const LinuxSizedFace& sized,
                          std::uint32_t glyph,
                          const RasterRequest& raster,
                          GlyphBitmap& bitmap)
    {
        auto* face = sized.face.get();

        // Light hinting: vertical only, so a stem keeps the subpixel position
        // it was asked for. Never LCD, which would bake one text colour into
        // the coverage the atlas caches.
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

        // Compared as the enum it is rather than as the byte FreeType stores
        // it in. A unity build concatenates this file after one that says
        // `using namespace eacp::GPU`, and a byte beside an enumerator makes
        // Clang instantiate the EDSL's constrained operator== with a builtin
        // type and reject it before the constraint is checked; two enums find
        // the builtin comparison and never look.
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

        // The glyph is drawn the fraction to the right of the pen's pixel and
        // the bitmap's bearings then measured from that pixel, rather than the
        // pixel-aligned glyph being placed at the fraction and resampled.
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

        // A glyph with nothing to draw is a space: valid, and it advances the
        // pen while rasterizing to nothing.
        if (source.width == 0 || source.rows == 0 || source.buffer == nullptr)
            return;

        bitmap.width = (int) source.width;
        bitmap.height = (int) source.rows;
        bitmap.pixels.assign(bitmap.width * bitmap.height, std::uint8_t {0});

        for (auto y = 0; y < bitmap.height; ++y)
        {
            // The pitch is an offset to add to walk down one row, and is
            // negative for a bitmap whose rows run upwards in memory.
            const auto* row = source.buffer + (std::ptrdiff_t) y * source.pitch;

            std::copy(row,
                      row + bitmap.width,
                      bitmap.pixels.data() + (std::size_t) y * bitmap.width);
        }
    }

    // A colour glyph, converted from FreeType's premultiplied BGRA to the
    // straight RGBA the atlas stores, and scaled from the strike it was drawn
    // at. Noto Color Emoji is one 109-pixel CBDT strike, so every size but
    // that one is a box filter away.
    //
    // The subpixel phase is deliberately ignored here: an emoji is a picture
    // rather than a stem, and resampling one four ways to place it a quarter
    // of a pixel differently buys nothing a reader could see.
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

    // One destination pixel as the average of the source pixels it covers,
    // averaged premultiplied - where compositing is linear - and written out
    // straight.
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

    // The bytes are copied first and the face opened over the copy, because
    // FreeType reads a memory face lazily and every FT_Face opened from it
    // later - by any rasterizer, at any size - reads the same buffer.
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

    // Registering the same face again is what a page reloaded does, and it
    // reports the face as it was rather than filing a second copy of it.
    for (const auto& existing: system.memoryFonts)
        if (Strings::equalsCaseInsensitive(existing->postScriptName,
                                           font->postScriptName))
            return RegisteredFont {existing->family, existing->postScriptName};

    auto names = RegisteredFont {font->family, font->postScriptName};

    system.memoryFonts.add(std::move(font));

    return names;
}
} // namespace eacp::Text

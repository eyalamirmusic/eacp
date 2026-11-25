#include "FontDiscovery-Linux.h"

#include <eacp/Core/Utils/Strings.h>

#include <fontconfig/fontconfig.h>

namespace eacp::Text::LinuxText
{
namespace
{
struct FontconfigPatternDeleter
{
    void operator()(FcPattern* pattern) const { FcPatternDestroy(pattern); }
};

struct FontconfigFontSetDeleter
{
    void operator()(FcFontSet* set) const { FcFontSetDestroy(set); }
};

struct FontconfigObjectSetDeleter
{
    void operator()(FcObjectSet* set) const { FcObjectSetDestroy(set); }
};

using FontconfigPattern = std::unique_ptr<FcPattern, FontconfigPatternDeleter>;
using FontconfigFontSet = std::unique_ptr<FcFontSet, FontconfigFontSetDeleter>;
using FontconfigObjectSet = std::unique_ptr<FcObjectSet, FontconfigObjectSetDeleter>;

std::string fontconfigString(const FcChar8* text)
{
    return text != nullptr ? std::string {(const char*) text} : std::string {};
}

FontconfigPattern fontconfigMatchFamily(const std::string& family)
{
    auto pattern = FontconfigPattern {FcPatternCreate()};

    if (!pattern)
        return {};

    FcPatternAddString(pattern.get(), FC_FAMILY, (const FcChar8*) family.c_str());
    FcConfigSubstitute(nullptr, pattern.get(), FcMatchPattern);
    FcDefaultSubstitute(pattern.get());

    auto status = FcResult {};

    return FontconfigPattern {FcFontMatch(nullptr, pattern.get(), &status)};
}

std::string fontconfigFamilyOf(FcPattern* pattern, const std::string& wanted)
{
    auto first = std::string {};

    for (auto index = 0;; ++index)
    {
        FcChar8* name = nullptr;

        if (FcPatternGetString(pattern, FC_FAMILY, index, &name) != FcResultMatch)
            break;

        const auto family = fontconfigString(name);

        if (Strings::equalsCaseInsensitive(family, wanted))
            return family;

        if (first.empty())
            first = family;
    }

    return first;
}

LinuxFamilyFace fontconfigFaceOfPattern(FcPattern* pattern)
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

    face.file = fontconfigString(file);

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

Vector<LinuxFamilyFace> fontconfigFacesOfFamily(const std::string& family)
{
    auto faces = Vector<LinuxFamilyFace> {};
    auto pattern = FontconfigPattern {FcPatternCreate()};
    auto objects = FontconfigObjectSet {
        FcObjectSetBuild(FC_FILE, FC_INDEX, FC_WEIGHT, FC_WIDTH, FC_SLANT, nullptr)};

    if (!pattern || !objects)
        return faces;

    FcPatternAddString(pattern.get(), FC_FAMILY, (const FcChar8*) family.c_str());

    auto set = FontconfigFontSet {FcFontList(nullptr, pattern.get(), objects.get())};

    if (!set)
        return faces;

    for (auto index = 0; index < set->nfont; ++index)
        if (auto face = fontconfigFaceOfPattern(set->fonts[index]); !face.isEmpty())
            faces.add(std::move(face));

    return faces;
}

std::string fontconfigFamilyOfPostScriptName(const std::string& name)
{
    auto pattern = FontconfigPattern {FcPatternCreate()};
    auto objects = FontconfigObjectSet {FcObjectSetBuild(FC_FAMILY, nullptr)};

    if (!pattern || !objects)
        return {};

    FcPatternAddString(
        pattern.get(), FC_POSTSCRIPT_NAME, (const FcChar8*) name.c_str());

    auto set = FontconfigFontSet {FcFontList(nullptr, pattern.get(), objects.get())};

    if (!set || set->nfont <= 0)
        return {};

    return fontconfigFamilyOf(set->fonts[0], {});
}

FontconfigFontSet fontconfigSortedFonts(const std::string& family,
                                        const FontVariant& variant,
                                        bool trim)
{
    auto pattern = FontconfigPattern {FcPatternCreate()};

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

    return FontconfigFontSet {FcFontSort(
        nullptr, pattern.get(), trim ? FcTrue : FcFalse, nullptr, &status)};
}

class FontconfigFallbackFonts final : public LinuxFallbackFonts
{
public:
    explicit FontconfigFallbackFonts(FontconfigFontSet setToUse)
        : set(std::move(setToUse))
    {
    }

    int size() const override { return set ? set->nfont : 0; }

    bool hasChar(int index, char32_t codepoint) const override
    {
        FcCharSet* charset = nullptr;

        if (FcPatternGetCharSet(set->fonts[index], FC_CHARSET, 0, &charset)
            != FcResultMatch)
            return false;

        return FcCharSetHasChar(charset, (FcChar32) codepoint) == FcTrue;
    }

    bool isColor(int index) const override
    {
        auto color = FcBool {FcFalse};

        return FcPatternGetBool(set->fonts[index], FC_COLOR, 0, &color)
                   == FcResultMatch
               && color == FcTrue;
    }

    LinuxFamilyFace faceAt(int index) const override
    {
        return fontconfigFaceOfPattern(set->fonts[index]);
    }

private:
    FontconfigFontSet set;
};
} // namespace

bool linuxStartFontDiscovery(LinuxFontSystem&)
{
    return FcInit() == FcTrue;
}

LinuxResolvedFamily linuxResolveFamily(const std::string& wanted)
{
    auto result = LinuxResolvedFamily {};
    auto matched = fontconfigMatchFamily(wanted);

    if (!matched)
        return result;

    result.family = fontconfigFamilyOf(matched.get(), wanted);

    if (!Strings::equalsCaseInsensitive(result.family, wanted))
        if (auto named = fontconfigFamilyOfPostScriptName(wanted); !named.empty())
            result.family = named;

    if (result.family.empty())
        return result;

    result.faces = fontconfigFacesOfFamily(result.family);

    // A family fontconfig only knows as a substitution lists no faces.
    if (result.faces.empty())
        if (auto face = fontconfigFaceOfPattern(matched.get()); !face.isEmpty())
            result.faces.add(std::move(face));

    return result;
}

std::unique_ptr<LinuxFallbackFonts> linuxFallbackFonts(const std::string& family,
                                                       const FontVariant& variant,
                                                       bool trim)
{
    return std::make_unique<FontconfigFallbackFonts>(
        fontconfigSortedFonts(family, variant, trim));
}
} // namespace eacp::Text::LinuxText

#include "FontDiscovery-Linux.h"

#include <eacp/Core/Utils/Strings.h>

#include <ResEmbed/ResEmbed.h>

namespace eacp::Text::LinuxText
{
namespace
{
// What CMake embedded under EACP_WEB_DEFAULT_FONTS (Lib/eacp/Text/CMakeLists.txt).
constexpr auto webBundledFontCategory = "EacpTextFonts";

constexpr auto webSansFamily = "DejaVu Sans";
constexpr auto webMonospaceFamily = "DejaVu Sans Mono";

bool webIsMonospaceName(const std::string& family)
{
    for (const auto* name: {"monospace",
                            "ui-monospace",
                            "Menlo",
                            "Monaco",
                            "Consolas",
                            "Courier",
                            "Courier New",
                            "SF Mono",
                            "Liberation Mono"})
        if (Strings::equalsCaseInsensitive(family, name))
            return true;

    return false;
}

LinuxFamilyFace webFaceOf(const LinuxMemoryFont& font)
{
    return {
        {}, 0, &font, font.weight, linuxNormalWidth, font.italic, font.weightVaries};
}

Vector<LinuxFamilyFace> webFacesOfFamily(const std::string& family)
{
    auto faces = Vector<LinuxFamilyFace> {};

    for (const auto& font: fontSystem().memoryFonts)
        if (Strings::equalsCaseInsensitive(font->family, family))
            faces.add(webFaceOf(*font));

    return faces;
}

FT_Face webCoverageOf(const LinuxMemoryFont& font)
{
    if (!font.coverage)
    {
        FT_Face raw = nullptr;

        if (FT_New_Memory_Face(
                fontSystem().library, font.data, (FT_Long) font.size, 0, &raw)
            == 0)
            font.coverage.reset(raw);
    }

    return font.coverage.get();
}

// Every family registered, bundled ones first, each as its face nearest the
// variant asked for. Nothing like fontconfig's sort: a page's fonts are few
// and named by the program, so their order is the one they were added in.
class WebFallbackFonts final : public LinuxFallbackFonts
{
public:
    WebFallbackFonts(const std::string& family, const FontVariant& variant)
    {
        for (const auto& font: fontSystem().memoryFonts)
        {
            if (Strings::equalsCaseInsensitive(font->family, family)
                || hasFamily(font->family))
                continue;

            const auto faces = webFacesOfFamily(font->family);

            if (const auto* nearest = linuxNearestFace(faces, variant))
                fonts.add(nearest->memory);
        }
    }

    int size() const override { return fonts.size(); }

    bool hasChar(int index, char32_t codepoint) const override
    {
        auto* face = webCoverageOf(*fonts[index]);

        return face != nullptr && FT_Get_Char_Index(face, (FT_ULong) codepoint) != 0;
    }

    bool isColor(int index) const override
    {
        auto* face = webCoverageOf(*fonts[index]);

        return face != nullptr && FT_HAS_COLOR(face);
    }

    LinuxFamilyFace faceAt(int index) const override
    {
        return webFaceOf(*fonts[index]);
    }

private:
    bool hasFamily(const std::string& family) const
    {
        for (const auto* font: fonts)
            if (Strings::equalsCaseInsensitive(font->family, family))
                return true;

        return false;
    }

    Vector<const LinuxMemoryFont*> fonts;
};
} // namespace

bool linuxStartFontDiscovery(LinuxFontSystem& system)
{
    for (const auto& [name, file]: ResEmbed::getCategory(webBundledFontCategory))
    {
        auto font = std::make_unique<LinuxMemoryFont>();
        font->data = file.data();
        font->size = file.getSize();

        linuxAddMemoryFont(system, std::move(font));
    }

    return true;
}

LinuxResolvedFamily linuxResolveFamily(const std::string& wanted)
{
    const auto* substitute =
        webIsMonospaceName(wanted) ? webMonospaceFamily : webSansFamily;

    if (auto faces = webFacesOfFamily(substitute); !faces.empty())
        return {substitute, faces};

    const auto& fonts = fontSystem().memoryFonts;

    if (fonts.empty())
        return {};

    return {fonts[0]->family, webFacesOfFamily(fonts[0]->family)};
}

std::unique_ptr<LinuxFallbackFonts>
    linuxFallbackFonts(const std::string& family, const FontVariant& variant, bool)
{
    return std::make_unique<WebFallbackFonts>(family, variant);
}
} // namespace eacp::Text::LinuxText

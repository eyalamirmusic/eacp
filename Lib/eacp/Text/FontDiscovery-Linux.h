#pragma once

#include "Font.h"
#include "GlyphRasterizer.h"

#include <ft2build.h>
#include FT_FREETYPE_H

#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>

// The FreeType rasterizer in GlyphRasterizer-Linux.cpp is shared by desktop
// Linux and the web, and is told where faces are by one of two discoveries:
// fontconfig on Linux (FontDiscovery-Linux.cpp) and the bundled-font registry
// on the web (FontDiscovery-Web.cpp). Everything here is that seam.
namespace eacp::Text::LinuxText
{
struct LinuxFaceDeleter
{
    void operator()(FT_FaceRec_* face) const { FT_Done_Face(face); }
};

using LinuxFace = std::unique_ptr<FT_FaceRec_, LinuxFaceDeleter>;

constexpr int linuxNormalWidth = 100;

// A face registered from memory. `data` is what FreeType reads, lazily, for
// the life of the process: `bytes` when registerMemoryFont copied it, or
// static data embedded in the program, which needs no copy.
struct LinuxMemoryFont
{
    Vector<std::uint8_t> bytes;
    const std::uint8_t* data = nullptr;
    int size = 0;

    std::string family;
    std::string postScriptName;
    int weight = 400;
    bool italic = false;
    bool weightVaries = false;

    mutable LinuxFace coverage;
};

// One FT_Library and one discovery for the process. Neither FreeType nor
// HarfBuzz's ft funcs are thread safe, so every call into them takes the mutex.
struct LinuxFontSystem
{
    LinuxFontSystem();

    std::mutex mutex;
    FT_Library library = nullptr;
    Vector<std::unique_ptr<LinuxMemoryFont>> memoryFonts;
    bool ready = false;
};

LinuxFontSystem& fontSystem();

// Fills in the names and style of a face whose data and size are set, and
// files it unless a face of that PostScript name is there already. Takes the
// system rather than asking fontSystem(), so the discovery can register while
// the system is still being built.
std::optional<RegisteredFont>
    linuxAddMemoryFont(LinuxFontSystem& system,
                       std::unique_ptr<LinuxMemoryFont> font);

// One face of a family: a file and its index for fontconfig's, a memory font
// for a registered one.
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

// CSS Fonts' matching: width, then slant, then weight.
const LinuxFamilyFace* linuxNearestFace(const Vector<LinuxFamilyFace>& faces,
                                        const FontVariant& variant);

struct LinuxResolvedFamily
{
    std::string family;
    Vector<LinuxFamilyFace> faces;
};

// The faces a codepoint the requested family lacks may come from, best first.
class LinuxFallbackFonts
{
public:
    virtual ~LinuxFallbackFonts() = default;

    virtual int size() const = 0;
    virtual bool hasChar(int index, char32_t codepoint) const = 0;
    virtual bool isColor(int index) const = 0;
    virtual LinuxFamilyFace faceAt(int index) const = 0;
};

// Called once, while the system is built and with FreeType up.
bool linuxStartFontDiscovery(LinuxFontSystem& system);

// A family no registered font answers to: the family itself, or the
// substitute the discovery chose, with every face it has.
LinuxResolvedFamily linuxResolveFamily(const std::string& wanted);

// `trim` drops what a face ranked above already covers, as FcFontSort does.
std::unique_ptr<LinuxFallbackFonts> linuxFallbackFonts(const std::string& family,
                                                       const FontVariant& variant,
                                                       bool trim);
} // namespace eacp::Text::LinuxText

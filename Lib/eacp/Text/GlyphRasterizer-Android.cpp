#include "GlyphRasterizer.h"
#include "Utf8.h"

#include <eacp/Core/Android/Jni.h>
#include <eacp/Core/Utils/Strings.h>

#include <android/bitmap.h>

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>

// Shaping is per code point, so there is no kerning, ligature or complex-script
// shaping, and the bitmap is ALPHA_8, so colour glyphs come out as masks.
// Android reports no names or axes for a memory font, so registerMemoryFont
// reads them from the face's own name and fvar tables.

namespace eacp::Text
{
namespace
{
struct AndroidGraphics
{
    void resolve(Jni::Lookup& java)
    {
        typeface = java.findClass("android/graphics/Typeface");
        paint = java.findClass("android/graphics/Paint");
        fontMetrics = java.findClass("android/graphics/Paint$FontMetrics");
        rect = java.findClass("android/graphics/Rect");
        bitmap = java.findClass("android/graphics/Bitmap");
        bitmapConfig = java.findClass("android/graphics/Bitmap$Config");
        canvas = java.findClass("android/graphics/Canvas");
        fontBuilder = java.findClass("android/graphics/fonts/Font$Builder");
        fontFamilyBuilder =
            java.findClass("android/graphics/fonts/FontFamily$Builder");
        fallbackBuilder =
            java.findClass("android/graphics/Typeface$CustomFallbackBuilder");

        defaultTypeface =
            java.staticObject(typeface, "DEFAULT", "Landroid/graphics/Typeface;");
        monospaceTypeface =
            java.staticObject(typeface, "MONOSPACE", "Landroid/graphics/Typeface;");
        alpha8 = java.staticObject(
            bitmapConfig, "ALPHA_8", "Landroid/graphics/Bitmap$Config;");

        createFromName = java.staticMethod(
            typeface, "create", "(Ljava/lang/String;I)Landroid/graphics/Typeface;");
        createWeighted = java.staticMethod(typeface,
                                           "create",
                                           "(Landroid/graphics/Typeface;IZ)"
                                           "Landroid/graphics/Typeface;");
        typefaceEquals = java.method(typeface, "equals", "(Ljava/lang/Object;)Z");

        paintInit = java.method(paint, "<init>", "(I)V");
        setTypeface =
            java.method(paint,
                        "setTypeface",
                        "(Landroid/graphics/Typeface;)Landroid/graphics/Typeface;");
        setTextSize = java.method(paint, "setTextSize", "(F)V");
        getFontMetrics = java.method(
            paint, "getFontMetrics", "()Landroid/graphics/Paint$FontMetrics;");
        measureText = java.method(paint, "measureText", "(Ljava/lang/String;)F");
        getTextBounds =
            java.method(paint,
                        "getTextBounds",
                        "(Ljava/lang/String;IILandroid/graphics/Rect;)V");
        hasGlyph = java.method(paint, "hasGlyph", "(Ljava/lang/String;)Z");

        ascent = java.field(fontMetrics, "ascent", "F");
        descent = java.field(fontMetrics, "descent", "F");
        leading = java.field(fontMetrics, "leading", "F");

        rectInit = java.method(rect, "<init>", "()V");
        left = java.field(rect, "left", "I");
        top = java.field(rect, "top", "I");
        right = java.field(rect, "right", "I");
        bottom = java.field(rect, "bottom", "I");

        createBitmap = java.staticMethod(
            bitmap,
            "createBitmap",
            "(IILandroid/graphics/Bitmap$Config;)Landroid/graphics/Bitmap;");
        recycle = java.method(bitmap, "recycle", "()V");

        canvasInit = java.method(canvas, "<init>", "(Landroid/graphics/Bitmap;)V");
        drawText = java.method(
            canvas, "drawText", "(Ljava/lang/String;FFLandroid/graphics/Paint;)V");
        setFontVariationSettings =
            java.method(paint, "setFontVariationSettings", "(Ljava/lang/String;)Z");

        fontBuilderInit =
            java.method(fontBuilder, "<init>", "(Ljava/nio/ByteBuffer;)V");
        fontBuild =
            java.method(fontBuilder, "build", "()Landroid/graphics/fonts/Font;");
        fontFamilyBuilderInit = java.method(
            fontFamilyBuilder, "<init>", "(Landroid/graphics/fonts/Font;)V");
        fontFamilyBuild = java.method(
            fontFamilyBuilder, "build", "()Landroid/graphics/fonts/FontFamily;");
        fallbackBuilderInit = java.method(
            fallbackBuilder, "<init>", "(Landroid/graphics/fonts/FontFamily;)V");
        setSystemFallback =
            java.method(fallbackBuilder,
                        "setSystemFallback",
                        "(Ljava/lang/String;)"
                        "Landroid/graphics/Typeface$CustomFallbackBuilder;");
        fallbackBuild =
            java.method(fallbackBuilder, "build", "()Landroid/graphics/Typeface;");
    }

    jclass typeface = nullptr;
    jclass paint = nullptr;
    jclass fontMetrics = nullptr;
    jclass rect = nullptr;
    jclass bitmap = nullptr;
    jclass bitmapConfig = nullptr;
    jclass canvas = nullptr;
    jclass fontBuilder = nullptr;
    jclass fontFamilyBuilder = nullptr;
    jclass fallbackBuilder = nullptr;

    jobject defaultTypeface = nullptr;
    jobject monospaceTypeface = nullptr;
    jobject alpha8 = nullptr;

    jmethodID createFromName = nullptr;
    jmethodID createWeighted = nullptr;
    jmethodID typefaceEquals = nullptr;
    jmethodID paintInit = nullptr;
    jmethodID setTypeface = nullptr;
    jmethodID setTextSize = nullptr;
    jmethodID getFontMetrics = nullptr;
    jmethodID measureText = nullptr;
    jmethodID getTextBounds = nullptr;
    jmethodID hasGlyph = nullptr;
    jmethodID rectInit = nullptr;
    jmethodID createBitmap = nullptr;
    jmethodID recycle = nullptr;
    jmethodID canvasInit = nullptr;
    jmethodID drawText = nullptr;
    jmethodID setFontVariationSettings = nullptr;
    jmethodID fontBuilderInit = nullptr;
    jmethodID fontBuild = nullptr;
    jmethodID fontFamilyBuilderInit = nullptr;
    jmethodID fontFamilyBuild = nullptr;
    jmethodID fallbackBuilderInit = nullptr;
    jmethodID setSystemFallback = nullptr;
    jmethodID fallbackBuild = nullptr;

    jfieldID ascent = nullptr;
    jfieldID descent = nullptr;
    jfieldID leading = nullptr;
    jfieldID left = nullptr;
    jfieldID top = nullptr;
    jfieldID right = nullptr;
    jfieldID bottom = nullptr;
};

constexpr jint paintAntiAliasFlag = 0x01;
constexpr jint paintSubpixelTextFlag = 0x80;

// Grayscale coverage at fractional positions and advances, which is what the
// atlas's phases need.
constexpr jint paintFlags = paintAntiAliasFlag | paintSubpixelTextFlag;

// Families a caller asks for meaning "the fixed-pitch face": the other
// platforms' stock ones, which Android does not ship, and its own alias.
bool isMonospaceFamily(const std::string& family)
{
    for (const auto* name: {"monospace",
                            "Menlo",
                            "Menlo-Regular",
                            "Menlo-Bold",
                            "Monaco",
                            "Consolas",
                            "DejaVu Sans Mono",
                            "Courier",
                            "Courier New",
                            "Droid Sans Mono",
                            "Roboto Mono"})
        if (Strings::equalsCaseInsensitive(family, name))
            return true;

    return false;
}

bool isGenericFamily(const std::string& family)
{
    for (const auto* name: {"sans-serif", "Roboto", "default", ""})
        if (Strings::equalsCaseInsensitive(family, name))
            return true;

    return false;
}

std::u16string utf16Of(char32_t codepoint)
{
    if (codepoint < 0x10000)
        return std::u16string(1, (char16_t) codepoint);

    const auto offset = codepoint - 0x10000;

    return {(char16_t) (0xD800 + (offset >> 10)),
            (char16_t) (0xDC00 + (offset & 0x3FF))};
}

struct VariantKey
{
    int weight = 400;
    bool italic = false;

    bool operator==(const VariantKey&) const = default;
};

struct VariantKeyHash
{
    std::size_t operator()(const VariantKey& key) const
    {
        return std::hash<int>()(key.weight * 2 + (key.italic ? 1 : 0));
    }
};

// Android hints every advance to a whole pixel at the size it measures at, so a
// line measured at the size being rasterized is a different width in points at
// every scale, and text that fits at one wraps at another. Layout is measured
// at this size instead and scaled to the one asked for, where a pixel of
// rounding is a fraction of a percent of an em.
constexpr auto layoutPixelSize = 1024.f;

struct AndroidVariant
{
    jobject paint = nullptr;

    // At layoutPixelSize; layoutScale takes what it measures to the
    // rasterizing size.
    jobject layoutPaint = nullptr;
    float layoutScale = 1.f;
    FontMetrics metrics;
    std::unordered_map<char32_t, float> advances;
};

constexpr std::uint32_t sfntTag(char a, char b, char c, char d)
{
    return ((std::uint32_t) a << 24) | ((std::uint32_t) b << 16)
           | ((std::uint32_t) c << 8) | (std::uint32_t) d;
}

constexpr auto weightAxisTag = sfntTag('w', 'g', 'h', 't');
constexpr auto italicAxisTag = sfntTag('i', 't', 'a', 'l');
constexpr auto slantAxisTag = sfntTag('s', 'l', 'n', 't');
constexpr auto opticalSizeAxisTag = sfntTag('o', 'p', 's', 'z');

constexpr std::uint16_t typographicFamilyNameId = 16;
constexpr std::uint16_t familyNameId = 1;
constexpr std::uint16_t postScriptNameId = 6;

// slnt counts degrees anticlockwise, so CSS's oblique is negative.
constexpr float obliqueSlant = -14.f;

// Reads past the end answer 0, so a truncated file reads as one with no tables.
struct SfntReader
{
    bool has(std::size_t offset, std::size_t length) const
    {
        return offset <= size && length <= size - offset;
    }

    std::uint16_t u16(std::size_t offset) const
    {
        if (!has(offset, 2))
            return 0;

        return (std::uint16_t) ((bytes[offset] << 8) | bytes[offset + 1]);
    }

    std::uint32_t u32(std::size_t offset) const
    {
        return ((std::uint32_t) u16(offset) << 16) | u16(offset + 2);
    }

    const std::uint8_t* bytes = nullptr;
    std::size_t size = 0;
};

struct SfntTable
{
    std::size_t offset = 0;
    std::size_t length = 0;

    bool found() const { return length > 0; }
};

// Font.Builder loads a collection's first face.
std::size_t firstFaceOffset(const SfntReader& font)
{
    return font.u32(0) == sfntTag('t', 't', 'c', 'f') ? font.u32(12) : 0;
}

SfntTable findTable(const SfntReader& font, std::uint32_t tag)
{
    const auto face = firstFaceOffset(font);
    const auto count = font.u16(face + 4);

    for (auto index = 0; index < count; ++index)
    {
        const auto record = face + 12 + (std::size_t) index * 16;

        if (!font.has(record, 16))
            break;

        if (font.u32(record) != tag)
            continue;

        const auto table = SfntTable {font.u32(record + 8), font.u32(record + 12)};

        return font.has(table.offset, table.length) ? table : SfntTable {};
    }

    return {};
}

std::string decodeUtf16BigEndian(const SfntReader& font,
                                 std::size_t offset,
                                 std::size_t length)
{
    auto result = std::string {};
    auto index = std::size_t {};

    const auto nextUnit = [&font, &index, offset]
    {
        const auto unit = font.u16(offset + index);
        index += 2;
        return (char32_t) unit;
    };

    while (index + 1 < length)
    {
        auto codepoint = nextUnit();

        if (codepoint >= 0xD800 && codepoint < 0xDC00 && index + 1 < length)
            codepoint =
                0x10000 + ((codepoint - 0xD800) << 10) + (nextUnit() - 0xDC00);

        char encoded[4] = {};
        result.append(encoded, (std::size_t) encodeUtf8(codepoint, encoded));
    }

    return result;
}

std::string
    decodeSingleByte(const SfntReader& font, std::size_t offset, std::size_t length)
{
    auto result = std::string {};

    for (auto index = std::size_t {}; index < length; ++index)
    {
        char encoded[4] = {};
        result.append(encoded,
                      (std::size_t) encodeUtf8(font.bytes[offset + index], encoded));
    }

    return result;
}

// Windows Unicode US English first, which is what CoreText, DirectWrite and
// FreeType report.
int nameRecordRank(std::uint16_t platform,
                   std::uint16_t encoding,
                   std::uint16_t language)
{
    if (platform == 3 && (encoding == 1 || encoding == 10))
        return language == 0x409 ? 4 : 3;

    if (platform == 0)
        return 2;

    if (platform == 1 && encoding == 0)
        return 1;

    return 0;
}

std::string nameOf(const SfntReader& font, const SfntTable& table, std::uint16_t id)
{
    const auto count = font.u16(table.offset + 2);
    const auto strings = table.offset + font.u16(table.offset + 4);
    auto best = std::string {};
    auto bestRank = 0;

    for (auto index = 0; index < count; ++index)
    {
        const auto record = table.offset + 6 + (std::size_t) index * 12;

        if (!font.has(record, 12))
            break;

        const auto platform = font.u16(record);
        const auto rank =
            nameRecordRank(platform, font.u16(record + 2), font.u16(record + 4));
        const auto length = (std::size_t) font.u16(record + 8);
        const auto start = strings + font.u16(record + 10);

        if (font.u16(record + 6) != id || rank <= bestRank
            || !font.has(start, length))
            continue;

        auto decoded = platform == 1 ? decodeSingleByte(font, start, length)
                                     : decodeUtf16BigEndian(font, start, length);

        if (decoded.empty())
            continue;

        best = decoded;
        bestRank = rank;
    }

    return best;
}

struct AndroidMemoryFont
{
    // The Typeface reads these lazily through a direct ByteBuffer.
    Vector<std::uint8_t> bytes;
    std::string family;
    std::string postScriptName;
    int weight = 400;
    bool italic = false;
    bool weightAxis = false;
    bool italicAxis = false;
    bool slantAxis = false;
    bool opticalSizeAxis = false;
    jobject typeface = nullptr;
};

// Entries are never removed or changed, so a pointer to one is safe unlocked.
struct AndroidMemoryFonts
{
    std::mutex mutex;
    Vector<std::unique_ptr<AndroidMemoryFont>> fonts;
};

AndroidMemoryFonts& memoryFonts()
{
    static auto* instance = new AndroidMemoryFonts {};

    return *instance;
}

void readAxes(const SfntReader& font, AndroidMemoryFont& into)
{
    const auto fvar = findTable(font, sfntTag('f', 'v', 'a', 'r'));

    if (!fvar.found())
        return;

    const auto axes = fvar.offset + font.u16(fvar.offset + 4);
    const auto count = font.u16(fvar.offset + 8);
    const auto axisSize = (std::size_t) font.u16(fvar.offset + 10);

    for (auto index = 0; index < count; ++index)
    {
        const auto tag = font.u32(axes + (std::size_t) index * axisSize);

        into.weightAxis |= tag == weightAxisTag;
        into.italicAxis |= tag == italicAxisTag;
        into.slantAxis |= tag == slantAxisTag;
        into.opticalSizeAxis |= tag == opticalSizeAxisTag;
    }
}

std::unique_ptr<AndroidMemoryFont> describeFont(const SfntReader& font)
{
    const auto names = findTable(font, sfntTag('n', 'a', 'm', 'e'));

    if (!names.found())
        return nullptr;

    auto described = std::make_unique<AndroidMemoryFont>();
    described->family = nameOf(font, names, typographicFamilyNameId);

    if (described->family.empty())
        described->family = nameOf(font, names, familyNameId);

    described->postScriptName = nameOf(font, names, postScriptNameId);

    if (described->family.empty() || described->postScriptName.empty())
        return nullptr;

    if (const auto os2 = findTable(font, sfntTag('O', 'S', '/', '2')); os2.found())
    {
        if (const auto weight = font.u16(os2.offset + 4); weight > 0)
            described->weight = weightClass(weight);

        described->italic = (font.u16(os2.offset + 62) & 1) != 0;
    }

    readAxes(font, *described);

    return described;
}

jobject
    buildTypeface(JNIEnv* env, const AndroidGraphics& java, AndroidMemoryFont& font)
{
    auto frame = Jni::LocalFrame {env};
    auto* buffer =
        env->NewDirectByteBuffer(font.bytes.data(), (jlong) font.bytes.size());

    if (Jni::failed(env) || buffer == nullptr)
        return nullptr;

    auto* fontBuilder =
        env->NewObject(java.fontBuilder, java.fontBuilderInit, buffer);
    auto* built = Jni::failed(env) || fontBuilder == nullptr
                      ? nullptr
                      : env->CallObjectMethod(fontBuilder, java.fontBuild);

    if (Jni::failed(env) || built == nullptr)
        return nullptr;

    auto* familyBuilder =
        env->NewObject(java.fontFamilyBuilder, java.fontFamilyBuilderInit, built);
    auto* family = Jni::failed(env) || familyBuilder == nullptr
                       ? nullptr
                       : env->CallObjectMethod(familyBuilder, java.fontFamilyBuild);

    if (Jni::failed(env) || family == nullptr)
        return nullptr;

    auto* fallbackBuilder =
        env->NewObject(java.fallbackBuilder, java.fallbackBuilderInit, family);

    if (Jni::failed(env) || fallbackBuilder == nullptr)
        return nullptr;

    auto* fallbackFamily = Jni::toJava(env, std::string_view {"sans-serif"});

    if (Jni::failed(env))
        return nullptr;

    env->CallObjectMethod(fallbackBuilder, java.setSystemFallback, fallbackFamily);

    if (Jni::failed(env))
        return nullptr;

    auto* typeface = env->CallObjectMethod(fallbackBuilder, java.fallbackBuild);

    if (Jni::failed(env) || typeface == nullptr)
        return nullptr;

    return env->NewGlobalRef(typeface);
}

Vector<const AndroidMemoryFont*> registeredFamilyOf(const std::string& name)
{
    auto& registry = memoryFonts();
    const auto lock = std::lock_guard {registry.mutex};
    auto family = std::string {};

    for (const auto& font: registry.fonts)
        if (Strings::equalsCaseInsensitive(font->family, name)
            || Strings::equalsCaseInsensitive(font->postScriptName, name))
        {
            family = font->family;
            break;
        }

    auto faces = Vector<const AndroidMemoryFont*> {};

    if (family.empty())
        return faces;

    for (const auto& font: registry.fonts)
        if (Strings::equalsCaseInsensitive(font->family, family))
            faces.add(font.get());

    return faces;
}

bool suppliesSlant(const AndroidMemoryFont& face)
{
    return face.italic || face.italicAxis || face.slantAxis;
}

const AndroidMemoryFont* nearestFace(const Vector<const AndroidMemoryFont*>& faces,
                                     const VariantKey& key)
{
    const AndroidMemoryFont* nearest = nullptr;
    auto nearestCost = 0;

    for (const auto* face: faces)
    {
        const auto slantCost = key.italic == suppliesSlant(*face) ? 0 : 1000;
        const auto weightCost =
            face->weightAxis ? 0 : std::abs(face->weight - key.weight);
        const auto cost = slantCost + weightCost;

        if (nearest == nullptr || cost < nearestCost)
        {
            nearest = face;
            nearestCost = cost;
        }
    }

    return nearest;
}

// opsz follows the point size, not the pixel size, so the design does not
// change with the display.
std::string variationSettings(const AndroidMemoryFont& face,
                              const VariantKey& key,
                              float pointSize)
{
    auto settings = std::string {};

    const auto add = [&settings](const char* tag, float value)
    {
        if (!settings.empty())
            settings += ", ";

        settings += "'" + std::string {tag} + "' " + std::to_string(value);
    };

    const auto wantsSlant = key.italic && !face.italic;

    if (face.weightAxis)
        add("wght", (float) key.weight);

    if (wantsSlant && face.italicAxis)
        add("ital", 1.f);
    else if (wantsSlant && face.slantAxis)
        add("slnt", obliqueSlant);

    if (face.opticalSizeAxis)
        add("opsz", pointSize);

    return settings;
}
} // namespace

struct GlyphRasterizer::Native
{
    explicit Native(const FontRequest& requestToUse)
        : request(requestToUse)
    {
        auto* env = Jni::currentEnv();

        if (env == nullptr)
            return;

        java = Jni::resolveOnce<AndroidGraphics>(env);

        if (java == nullptr)
            return;

        auto frame = Jni::LocalFrame {env};

        resolve(env);
        valid = base != nullptr && variant(env, {}) != nullptr;
    }

    ~Native()
    {
        auto* env = Jni::currentEnv();

        if (env == nullptr)
            return;

        for (auto& [key, face]: variants)
        {
            env->DeleteGlobalRef(face.paint);
            env->DeleteGlobalRef(face.layoutPaint);
        }

        if (base != nullptr)
            env->DeleteGlobalRef(base);
    }

    void resolve(JNIEnv* env)
    {
        memoryFaces = registeredFamilyOf(request.family);

        if (!memoryFaces.empty())
        {
            base = env->NewGlobalRef(memoryFaces[0]->typeface);
            resolved = memoryFaces[0]->family;
            return;
        }

        if (isMonospaceFamily(request.family))
        {
            base = env->NewGlobalRef(java->monospaceTypeface);
            resolved = "monospace";
            return;
        }

        auto* name = env->NewStringUTF(request.family.c_str());
        auto* typeface =
            Jni::failed(env)
                ? nullptr
                : env->CallStaticObjectMethod(
                      java->typeface, java->createFromName, name, (jint) 0);

        if (Jni::failed(env) || typeface == nullptr)
            typeface = java->defaultTypeface;

        // Typeface.create hands back the default face for a name it does not
        // know, which is the substitute to report.
        auto isDefault = env->CallBooleanMethod(
            typeface, java->typefaceEquals, java->defaultTypeface);
        auto substituted =
            !Jni::failed(env) && isDefault && !isGenericFamily(request.family);

        base = env->NewGlobalRef(typeface);
        resolved =
            substituted || request.family.empty() ? "sans-serif" : request.family;
    }

    AndroidVariant* variant(JNIEnv* env, const FontVariant& wanted) const
    {
        const auto key = VariantKey {weightClass(wanted.weight), wanted.italic};

        if (auto found = variants.find(key); found != variants.end())
            return &found->second;

        auto frame = Jni::LocalFrame {env};
        const auto* memoryFace = nearestFace(memoryFaces, key);
        auto* source = memoryFace != nullptr ? memoryFace->typeface : base;
        auto weight = key.weight;
        auto italic = key.italic;

        if (memoryFace != nullptr)
        {
            weight = memoryFace->weightAxis ? memoryFace->weight : key.weight;
            italic = key.italic && !suppliesSlant(*memoryFace);
        }

        auto* typeface = env->CallStaticObjectMethod(java->typeface,
                                                     java->createWeighted,
                                                     source,
                                                     (jint) weight,
                                                     (jboolean) italic);

        if (Jni::failed(env) || typeface == nullptr)
            typeface = source;

        auto* paint = makePaint(env, typeface, memoryFace, key, request.pixelSize());
        auto* layoutPaint =
            makePaint(env, typeface, memoryFace, key, layoutPixelSize);

        if (paint == nullptr || layoutPaint == nullptr)
            return nullptr;

        auto* metrics = env->CallObjectMethod(layoutPaint, java->getFontMetrics);

        if (Jni::failed(env) || metrics == nullptr)
            return nullptr;

        auto face = AndroidVariant {};
        face.layoutScale = request.pixelSize() / layoutPixelSize;
        face.metrics.ascent =
            -env->GetFloatField(metrics, java->ascent) * face.layoutScale;
        face.metrics.descent =
            env->GetFloatField(metrics, java->descent) * face.layoutScale;
        face.metrics.leading =
            std::max(0.f, env->GetFloatField(metrics, java->leading))
            * face.layoutScale;

        auto* letter = Jni::toJava(env, u"M");

        if (Jni::failed(env))
            return nullptr;

        face.metrics.advance =
            env->CallFloatMethod(layoutPaint, java->measureText, letter)
            * face.layoutScale;

        if (Jni::failed(env))
            return nullptr;

        face.paint = env->NewGlobalRef(paint);
        face.layoutPaint = env->NewGlobalRef(layoutPaint);

        return &variants.emplace(key, std::move(face)).first->second;
    }

    jobject makePaint(JNIEnv* env,
                      jobject typeface,
                      const AndroidMemoryFont* memoryFace,
                      const VariantKey& key,
                      float pixelSize) const
    {
        auto* paint = env->NewObject(java->paint, java->paintInit, paintFlags);

        if (Jni::failed(env) || paint == nullptr)
            return nullptr;

        env->CallObjectMethod(paint, java->setTypeface, typeface);

        if (Jni::failed(env))
            return nullptr;

        if (memoryFace != nullptr)
            if (!applyVariation(env, paint, *memoryFace, key))
                return nullptr;

        env->CallVoidMethod(paint, java->setTextSize, (jfloat) pixelSize);

        return Jni::failed(env) ? nullptr : paint;
    }

    bool applyVariation(JNIEnv* env,
                        jobject paint,
                        const AndroidMemoryFont& face,
                        const VariantKey& key) const
    {
        const auto settings = variationSettings(face, key, request.pointSize);

        if (settings.empty())
            return true;

        auto* text = Jni::toJava(env, std::string_view {settings});

        if (Jni::failed(env))
            return false;

        env->CallBooleanMethod(paint, java->setFontVariationSettings, text);

        return !Jni::failed(env);
    }

    float advanceOf(JNIEnv* env, AndroidVariant& face, char32_t codepoint) const
    {
        if (auto found = face.advances.find(codepoint); found != face.advances.end())
            return found->second;

        auto frame = Jni::LocalFrame {env};
        auto* text = Jni::toJava(env, utf16Of(codepoint));
        auto advance =
            Jni::failed(env)
                ? 0.f
                : env->CallFloatMethod(face.layoutPaint, java->measureText, text)
                      * face.layoutScale;

        if (Jni::failed(env))
            advance = 0.f;

        face.advances.emplace(codepoint, advance);

        return advance;
    }

    FontMetrics metrics(const FontVariant& wanted) const
    {
        const auto lock = std::lock_guard {mutex};
        auto* env = valid ? Jni::currentEnv() : nullptr;
        auto* face = env != nullptr ? variant(env, wanted) : nullptr;

        return face != nullptr ? face->metrics : FontMetrics {};
    }

    ShapedRun shape(std::string_view text, const FontVariant& wanted) const
    {
        const auto lock = std::lock_guard {mutex};
        auto result = ShapedRun {};
        auto* env = valid ? Jni::currentEnv() : nullptr;
        auto* face = env != nullptr ? variant(env, wanted) : nullptr;

        if (face == nullptr)
            return result;

        auto index = 0;

        while (index < (int) text.size())
        {
            const auto cluster = index;
            const auto codepoint = decodeUtf8(text, index);

            result.glyphs.add(ShapedGlyph {
                {(std::uint32_t) codepoint, 0}, result.advance, 0.f, cluster});
            result.advance += advanceOf(env, *face, codepoint);
        }

        return result;
    }

    GlyphBitmap rasterize(GlyphKey key,
                          const FontVariant& wanted,
                          const RasterRequest& raster) const
    {
        const auto lock = std::lock_guard {mutex};
        auto result = GlyphBitmap {};
        auto* env = valid ? Jni::currentEnv() : nullptr;
        auto* face = env != nullptr ? variant(env, wanted) : nullptr;

        if (face == nullptr || key.font != 0)
            return result;

        const auto codepoint = (char32_t) key.glyph;
        auto frame = Jni::LocalFrame {env};
        const auto utf16 = utf16Of(codepoint);
        auto* text = Jni::toJava(env, utf16);

        if (Jni::failed(env))
            return result;

        auto present = env->CallBooleanMethod(face->paint, java->hasGlyph, text);

        if (Jni::failed(env) || !present)
            return result;

        result.valid = true;
        result.advance = advanceOf(env, *face, codepoint);

        auto* bounds = env->NewObject(java->rect, java->rectInit);

        if (Jni::failed(env) || bounds == nullptr)
            return result;

        env->CallVoidMethod(face->paint,
                            java->getTextBounds,
                            text,
                            (jint) 0,
                            (jint) utf16.size(),
                            bounds);

        if (Jni::failed(env))
            return result;

        const auto boundsLeft = env->GetIntField(bounds, java->left);
        const auto boundsTop = env->GetIntField(bounds, java->top);
        const auto boundsRight = env->GetIntField(bounds, java->right);
        const auto boundsBottom = env->GetIntField(bounds, java->bottom);

        if (boundsRight <= boundsLeft || boundsBottom <= boundsTop)
            return result;

        // A pixel of room each way for the antialiased edge the integer bounds
        // leave out, and one more on the right for the subpixel shift.
        const auto left = boundsLeft - 1;
        const auto top = boundsTop - 1;
        const auto width = boundsRight + 2 - left;
        const auto height = boundsBottom + 1 - top;

        auto* bitmap = env->CallStaticObjectMethod(java->bitmap,
                                                   java->createBitmap,
                                                   (jint) width,
                                                   (jint) height,
                                                   java->alpha8);

        if (Jni::failed(env) || bitmap == nullptr)
            return result;

        auto* canvas = env->NewObject(java->canvas, java->canvasInit, bitmap);

        if (!Jni::failed(env) && canvas != nullptr)
        {
            env->CallVoidMethod(canvas,
                                java->drawText,
                                text,
                                (jfloat) (raster.subpixelX - (float) left),
                                (jfloat) -top,
                                face->paint);

            if (!Jni::failed(env))
                copyPixels(env, bitmap, width, height, result);
        }

        env->CallVoidMethod(bitmap, java->recycle);
        Jni::failed(env);

        result.bearingX = (float) left;
        result.bearingY = (float) -top;

        return result;
    }

    static void copyPixels(
        JNIEnv* env, jobject bitmap, int width, int height, GlyphBitmap& into)
    {
        auto info = AndroidBitmapInfo {};
        void* pixels = nullptr;

        if (AndroidBitmap_getInfo(env, bitmap, &info)
                != ANDROID_BITMAP_RESULT_SUCCESS
            || info.format != ANDROID_BITMAP_FORMAT_A_8
            || AndroidBitmap_lockPixels(env, bitmap, &pixels)
                   != ANDROID_BITMAP_RESULT_SUCCESS)
            return;

        into.width = width;
        into.height = height;
        into.format = GlyphFormat::Mask;
        into.pixels.resize(width * height);

        const auto* source = static_cast<const std::uint8_t*>(pixels);

        for (auto row = 0; row < height; ++row)
            std::copy_n(
                source + row * info.stride, width, into.pixels.data() + row * width);

        AndroidBitmap_unlockPixels(env, bitmap);
    }

    FontRequest request;
    std::string resolved;
    bool valid = false;

    const AndroidGraphics* java = nullptr;
    jobject base = nullptr;
    Vector<const AndroidMemoryFont*> memoryFaces;
    mutable std::unordered_map<VariantKey, AndroidVariant, VariantKeyHash> variants;
    mutable std::mutex mutex;
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
    return impl->rasterize({(std::uint32_t) codepoint, 0}, variantOf(style), {});
}

const FontRequest& GlyphRasterizer::request() const
{
    return impl->request;
}

std::optional<RegisteredFont> registerMemoryFont(const void* data, int size)
{
    if (data == nullptr || size <= 0)
        return std::nullopt;

    const auto* bytes = static_cast<const std::uint8_t*>(data);
    auto font = describeFont(SfntReader {bytes, (std::size_t) size});

    if (font == nullptr)
        return std::nullopt;

    auto& registry = memoryFonts();
    const auto lock = std::lock_guard {registry.mutex};

    for (const auto& existing: registry.fonts)
        if (Strings::equalsCaseInsensitive(existing->postScriptName,
                                           font->postScriptName))
            return RegisteredFont {existing->family, existing->postScriptName};

    auto* env = Jni::currentEnv();
    const auto* java =
        env != nullptr ? Jni::resolveOnce<AndroidGraphics>(env) : nullptr;

    if (java == nullptr)
        return std::nullopt;

    font->bytes.assign(bytes, bytes + size);
    font->typeface = buildTypeface(env, *java, *font);

    if (font->typeface == nullptr)
        return std::nullopt;

    auto names = RegisteredFont {font->family, font->postScriptName};
    registry.fonts.add(std::move(font));

    return names;
}
} // namespace eacp::Text

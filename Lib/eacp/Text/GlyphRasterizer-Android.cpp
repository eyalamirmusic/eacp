#include "GlyphRasterizer.h"
#include "Utf8.h"

#include <eacp/Core/Utils/Logging.h>
#include <eacp/Core/Utils/Strings.h>
#include <eacp/Graphics/Window/Android.h>

#include <android/bitmap.h>
#include <android_native_app_glue.h>
#include <jni.h>

#include <algorithm>
#include <mutex>
#include <string>
#include <unordered_map>

// Shaping is per code point, so there is no kerning, ligature or complex-script
// shaping, and the bitmap is ALPHA_8, so colour glyphs come out as masks.

namespace eacp::Text
{
namespace
{
JavaVM* javaVM()
{
    auto* app = Graphics::Android::getApp();

    if (app == nullptr || app->activity == nullptr)
        return nullptr;

    return app->activity->vm;
}

// Attached once per thread and detached when the thread ends, so a thread that
// rasterizes a screenful of glyphs does not attach and detach per glyph.
struct ThreadAttachment
{
    ~ThreadAttachment()
    {
        if (vm != nullptr)
            vm->DetachCurrentThread();
    }

    JavaVM* vm = nullptr;
};

JNIEnv* currentEnv()
{
    auto* vm = javaVM();

    if (vm == nullptr)
        return nullptr;

    auto* env = static_cast<JNIEnv*>(nullptr);

    if (vm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6) == JNI_OK)
        return env;

    if (vm->AttachCurrentThread(&env, nullptr) != JNI_OK)
        return nullptr;

    thread_local auto attachment = ThreadAttachment {};
    attachment.vm = vm;

    return env;
}

bool failed(JNIEnv* env)
{
    if (!env->ExceptionCheck())
        return false;

    env->ExceptionClear();
    return true;
}

constexpr jint localReferencesPerCall = 32;

// Every local reference a call makes is released when it returns: the render
// thread never goes back to Java, so nothing else would release them.
struct LocalFrame
{
    explicit LocalFrame(JNIEnv* envToUse)
        : env(envToUse)
        , pushed(env->PushLocalFrame(localReferencesPerCall) == 0)
    {
        if (!pushed)
            failed(env);
    }

    ~LocalFrame()
    {
        if (pushed)
            env->PopLocalFrame(nullptr);
    }

    JNIEnv* env;
    bool pushed;
};

struct AndroidGraphics
{
    bool load(JNIEnv* env)
    {
        auto frame = LocalFrame {env};

        auto global = [env](const char* name)
        {
            auto* local = env->FindClass(name);
            return failed(env) || local == nullptr
                       ? nullptr
                       : static_cast<jclass>(env->NewGlobalRef(local));
        };

        typeface = global("android/graphics/Typeface");
        paint = global("android/graphics/Paint");
        fontMetrics = global("android/graphics/Paint$FontMetrics");
        rect = global("android/graphics/Rect");
        bitmap = global("android/graphics/Bitmap");
        bitmapConfig = global("android/graphics/Bitmap$Config");
        canvas = global("android/graphics/Canvas");

        if (typeface == nullptr || paint == nullptr || fontMetrics == nullptr
            || rect == nullptr || bitmap == nullptr || bitmapConfig == nullptr
            || canvas == nullptr)
            return false;

        auto staticObject = [env](jclass owner, const char* name, const char* type)
        {
            auto field = env->GetStaticFieldID(owner, name, type);
            auto* local =
                failed(env) ? nullptr : env->GetStaticObjectField(owner, field);
            return failed(env) || local == nullptr ? nullptr
                                                   : env->NewGlobalRef(local);
        };

        auto complete = true;

        auto method =
            [env, &complete](jclass owner, const char* name, const char* type)
        {
            auto id = env->GetMethodID(owner, name, type);
            complete = complete && !failed(env) && id != nullptr;
            return id;
        };

        auto staticMethod =
            [env, &complete](jclass owner, const char* name, const char* type)
        {
            auto id = env->GetStaticMethodID(owner, name, type);
            complete = complete && !failed(env) && id != nullptr;
            return id;
        };

        auto field =
            [env, &complete](jclass owner, const char* name, const char* type)
        {
            auto id = env->GetFieldID(owner, name, type);
            complete = complete && !failed(env) && id != nullptr;
            return id;
        };

        defaultTypeface =
            staticObject(typeface, "DEFAULT", "Landroid/graphics/Typeface;");
        monospaceTypeface =
            staticObject(typeface, "MONOSPACE", "Landroid/graphics/Typeface;");
        alpha8 = staticObject(
            bitmapConfig, "ALPHA_8", "Landroid/graphics/Bitmap$Config;");

        createFromName = staticMethod(
            typeface, "create", "(Ljava/lang/String;I)Landroid/graphics/Typeface;");
        createWeighted = staticMethod(typeface,
                                      "create",
                                      "(Landroid/graphics/Typeface;IZ)"
                                      "Landroid/graphics/Typeface;");
        typefaceEquals = method(typeface, "equals", "(Ljava/lang/Object;)Z");

        paintInit = method(paint, "<init>", "(I)V");
        setTypeface =
            method(paint,
                   "setTypeface",
                   "(Landroid/graphics/Typeface;)Landroid/graphics/Typeface;");
        setTextSize = method(paint, "setTextSize", "(F)V");
        getFontMetrics = method(
            paint, "getFontMetrics", "()Landroid/graphics/Paint$FontMetrics;");
        measureText = method(paint, "measureText", "(Ljava/lang/String;)F");
        getTextBounds = method(paint,
                               "getTextBounds",
                               "(Ljava/lang/String;IILandroid/graphics/Rect;)V");
        hasGlyph = method(paint, "hasGlyph", "(Ljava/lang/String;)Z");

        ascent = field(fontMetrics, "ascent", "F");
        descent = field(fontMetrics, "descent", "F");
        leading = field(fontMetrics, "leading", "F");

        rectInit = method(rect, "<init>", "()V");
        left = field(rect, "left", "I");
        top = field(rect, "top", "I");
        right = field(rect, "right", "I");
        bottom = field(rect, "bottom", "I");

        createBitmap = staticMethod(
            bitmap,
            "createBitmap",
            "(IILandroid/graphics/Bitmap$Config;)Landroid/graphics/Bitmap;");
        recycle = method(bitmap, "recycle", "()V");

        canvasInit = method(canvas, "<init>", "(Landroid/graphics/Bitmap;)V");
        drawText = method(
            canvas, "drawText", "(Ljava/lang/String;FFLandroid/graphics/Paint;)V");

        return complete && defaultTypeface != nullptr && monospaceTypeface != nullptr
               && alpha8 != nullptr;
    }

    jclass typeface = nullptr;
    jclass paint = nullptr;
    jclass fontMetrics = nullptr;
    jclass rect = nullptr;
    jclass bitmap = nullptr;
    jclass bitmapConfig = nullptr;
    jclass canvas = nullptr;

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

    jfieldID ascent = nullptr;
    jfieldID descent = nullptr;
    jfieldID leading = nullptr;
    jfieldID left = nullptr;
    jfieldID top = nullptr;
    jfieldID right = nullptr;
    jfieldID bottom = nullptr;
};

const AndroidGraphics* androidGraphics(JNIEnv* env)
{
    static auto once = std::once_flag {};
    static auto graphics = AndroidGraphics {};
    static auto loaded = false;

    std::call_once(once,
                   [env]
                   {
                       loaded = graphics.load(env);

                       if (!loaded)
                           LOG("Text: android.graphics did not load; no text "
                               "will be drawn");
                   });

    return loaded ? &graphics : nullptr;
}

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
                            "Roboto Mono",
                            defaultMonospaceFamily()})
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

jstring javaString(JNIEnv* env, std::u16string_view text)
{
    return env->NewString(reinterpret_cast<const jchar*>(text.data()),
                          (jsize) text.size());
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

struct AndroidVariant
{
    jobject paint = nullptr;
    FontMetrics metrics;
    std::unordered_map<char32_t, float> advances;
};
} // namespace

struct GlyphRasterizer::Native
{
    explicit Native(const FontRequest& requestToUse)
        : request(requestToUse)
    {
        auto* env = currentEnv();

        if (env == nullptr)
            return;

        java = androidGraphics(env);

        if (java == nullptr)
            return;

        auto frame = LocalFrame {env};

        resolve(env);
        valid = base != nullptr && variant(env, {}) != nullptr;
    }

    ~Native()
    {
        auto* env = currentEnv();

        if (env == nullptr)
            return;

        for (auto& [key, face]: variants)
            env->DeleteGlobalRef(face.paint);

        if (base != nullptr)
            env->DeleteGlobalRef(base);
    }

    void resolve(JNIEnv* env)
    {
        if (isMonospaceFamily(request.family))
        {
            base = env->NewGlobalRef(java->monospaceTypeface);
            resolved = "monospace";
            return;
        }

        auto* name = env->NewStringUTF(request.family.c_str());
        auto* typeface =
            failed(env) ? nullptr
                        : env->CallStaticObjectMethod(
                              java->typeface, java->createFromName, name, (jint) 0);

        if (failed(env) || typeface == nullptr)
            typeface = java->defaultTypeface;

        // Typeface.create hands back the default face for a name it does not
        // know, which is the substitute to report.
        auto isDefault = env->CallBooleanMethod(
            typeface, java->typefaceEquals, java->defaultTypeface);
        auto substituted =
            !failed(env) && isDefault && !isGenericFamily(request.family);

        base = env->NewGlobalRef(typeface);
        resolved =
            substituted || request.family.empty() ? "sans-serif" : request.family;
    }

    AndroidVariant* variant(JNIEnv* env, const FontVariant& wanted) const
    {
        const auto key = VariantKey {weightClass(wanted.weight), wanted.italic};

        if (auto found = variants.find(key); found != variants.end())
            return &found->second;

        auto frame = LocalFrame {env};

        auto* typeface = env->CallStaticObjectMethod(java->typeface,
                                                     java->createWeighted,
                                                     base,
                                                     (jint) key.weight,
                                                     (jboolean) key.italic);

        if (failed(env) || typeface == nullptr)
            typeface = base;

        auto* paint = env->NewObject(java->paint, java->paintInit, paintFlags);

        if (failed(env) || paint == nullptr)
            return nullptr;

        env->CallObjectMethod(paint, java->setTypeface, typeface);

        if (failed(env))
            return nullptr;

        env->CallVoidMethod(paint, java->setTextSize, (jfloat) request.pixelSize());

        if (failed(env))
            return nullptr;

        auto* metrics = env->CallObjectMethod(paint, java->getFontMetrics);

        if (failed(env) || metrics == nullptr)
            return nullptr;

        auto face = AndroidVariant {};
        face.metrics.ascent = -env->GetFloatField(metrics, java->ascent);
        face.metrics.descent = env->GetFloatField(metrics, java->descent);
        face.metrics.leading =
            std::max(0.f, env->GetFloatField(metrics, java->leading));

        auto* letter = javaString(env, u"M");

        if (failed(env))
            return nullptr;

        face.metrics.advance =
            env->CallFloatMethod(paint, java->measureText, letter);

        if (failed(env))
            return nullptr;

        face.paint = env->NewGlobalRef(paint);

        return &variants.emplace(key, std::move(face)).first->second;
    }

    float advanceOf(JNIEnv* env, AndroidVariant& face, char32_t codepoint) const
    {
        if (auto found = face.advances.find(codepoint); found != face.advances.end())
            return found->second;

        auto frame = LocalFrame {env};
        auto* text = javaString(env, utf16Of(codepoint));
        auto advance =
            failed(env) ? 0.f
                        : env->CallFloatMethod(face.paint, java->measureText, text);

        if (failed(env))
            advance = 0.f;

        face.advances.emplace(codepoint, advance);

        return advance;
    }

    FontMetrics metrics(const FontVariant& wanted) const
    {
        const auto lock = std::lock_guard {mutex};
        auto* env = valid ? currentEnv() : nullptr;
        auto* face = env != nullptr ? variant(env, wanted) : nullptr;

        return face != nullptr ? face->metrics : FontMetrics {};
    }

    ShapedRun shape(std::string_view text, const FontVariant& wanted) const
    {
        const auto lock = std::lock_guard {mutex};
        auto result = ShapedRun {};
        auto* env = valid ? currentEnv() : nullptr;
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
        auto* env = valid ? currentEnv() : nullptr;
        auto* face = env != nullptr ? variant(env, wanted) : nullptr;

        if (face == nullptr || key.font != 0)
            return result;

        const auto codepoint = (char32_t) key.glyph;
        auto frame = LocalFrame {env};
        const auto utf16 = utf16Of(codepoint);
        auto* text = javaString(env, utf16);

        if (failed(env))
            return result;

        auto present = env->CallBooleanMethod(face->paint, java->hasGlyph, text);

        if (failed(env) || !present)
            return result;

        result.valid = true;
        result.advance = advanceOf(env, *face, codepoint);

        auto* bounds = env->NewObject(java->rect, java->rectInit);

        if (failed(env) || bounds == nullptr)
            return result;

        env->CallVoidMethod(face->paint,
                            java->getTextBounds,
                            text,
                            (jint) 0,
                            (jint) utf16.size(),
                            bounds);

        if (failed(env))
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

        if (failed(env) || bitmap == nullptr)
            return result;

        auto* canvas = env->NewObject(java->canvas, java->canvasInit, bitmap);

        if (!failed(env) && canvas != nullptr)
        {
            env->CallVoidMethod(canvas,
                                java->drawText,
                                text,
                                (jfloat) (raster.subpixelX - (float) left),
                                (jfloat) -top,
                                face->paint);

            if (!failed(env))
                copyPixels(env, bitmap, width, height, result);
        }

        env->CallVoidMethod(bitmap, java->recycle);
        failed(env);

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

// Typeface.Builder takes a file or an asset, not bytes.
std::optional<RegisteredFont> registerMemoryFont(const void*, int)
{
    return std::nullopt;
}
} // namespace eacp::Text

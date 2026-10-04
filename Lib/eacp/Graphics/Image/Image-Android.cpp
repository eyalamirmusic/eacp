#include "ImageCodec.h"

#include <eacp/Core/Android/Jni.h>

#include <android/bitmap.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <string_view>

// An unpremultiplied ARGB_8888 bitmap is R,G,B,A in memory, the Image's layout.

namespace eacp::Graphics::detail
{
namespace
{
struct AndroidImageJava
{
    void resolve(Jni::Lookup& java)
    {
        bitmapFactory = java.findClass("android/graphics/BitmapFactory");
        options = java.findClass("android/graphics/BitmapFactory$Options");
        bitmap = java.findClass("android/graphics/Bitmap");
        bitmapConfig = java.findClass("android/graphics/Bitmap$Config");
        compressFormat = java.findClass("android/graphics/Bitmap$CompressFormat");
        byteStream = java.findClass("java/io/ByteArrayOutputStream");

        argb8888 = java.staticObject(
            bitmapConfig, "ARGB_8888", "Landroid/graphics/Bitmap$Config;");
        png = java.staticObject(
            compressFormat, "PNG", "Landroid/graphics/Bitmap$CompressFormat;");
        jpeg = java.staticObject(
            compressFormat, "JPEG", "Landroid/graphics/Bitmap$CompressFormat;");

        decodeByteArray =
            java.staticMethod(bitmapFactory,
                              "decodeByteArray",
                              "([BIILandroid/graphics/BitmapFactory$Options;)"
                              "Landroid/graphics/Bitmap;");

        optionsInit = java.method(options, "<init>", "()V");
        inPreferredConfig = java.field(
            options, "inPreferredConfig", "Landroid/graphics/Bitmap$Config;");
        inPremultiplied = java.field(options, "inPremultiplied", "Z");
        inScaled = java.field(options, "inScaled", "Z");

        createBitmap = java.staticMethod(
            bitmap,
            "createBitmap",
            "(IILandroid/graphics/Bitmap$Config;)Landroid/graphics/Bitmap;");
        setPremultiplied = java.method(bitmap, "setPremultiplied", "(Z)V");
        setHasAlpha = java.method(bitmap, "setHasAlpha", "(Z)V");
        compress = java.method(bitmap,
                               "compress",
                               "(Landroid/graphics/Bitmap$CompressFormat;I"
                               "Ljava/io/OutputStream;)Z");
        recycle = java.method(bitmap, "recycle", "()V");

        byteStreamInit = java.method(byteStream, "<init>", "(I)V");
        toByteArray = java.method(byteStream, "toByteArray", "()[B");
    }

    jclass bitmapFactory = nullptr;
    jclass options = nullptr;
    jclass bitmap = nullptr;
    jclass bitmapConfig = nullptr;
    jclass compressFormat = nullptr;
    jclass byteStream = nullptr;

    jobject argb8888 = nullptr;
    jobject png = nullptr;
    jobject jpeg = nullptr;

    jmethodID decodeByteArray = nullptr;
    jmethodID optionsInit = nullptr;
    jfieldID inPreferredConfig = nullptr;
    jfieldID inPremultiplied = nullptr;
    jfieldID inScaled = nullptr;

    jmethodID createBitmap = nullptr;
    jmethodID setPremultiplied = nullptr;
    jmethodID setHasAlpha = nullptr;
    jmethodID compress = nullptr;
    jmethodID recycle = nullptr;

    jmethodID byteStreamInit = nullptr;
    jmethodID toByteArray = nullptr;
};

constexpr auto initialEncodeCapacity = jint {64 * 1024};

struct JavaSession
{
    JNIEnv* env = nullptr;
    const AndroidImageJava* java = nullptr;
};

JavaSession openSession(std::string& error)
{
    auto* env = Jni::currentEnv();

    if (env == nullptr)
    {
        error = "no Java VM for the image codec";
        return {};
    }

    auto* java = Jni::resolveOnce<AndroidImageJava>(env);

    if (java == nullptr)
        error = "android.graphics is missing a class the image codec needs";

    return {env, java};
}

std::string javaError(JNIEnv* env, std::string_view fallback)
{
    auto message = Jni::takeException(env);
    return message.empty() ? std::string {fallback} : message;
}

class LockedPixels
{
public:
    LockedPixels(JNIEnv* envToUse, jobject bitmapToUse)
        : env(envToUse)
        , bitmap(bitmapToUse)
    {
        if (AndroidBitmap_getInfo(env, bitmap, &info)
                != ANDROID_BITMAP_RESULT_SUCCESS
            || info.format != ANDROID_BITMAP_FORMAT_RGBA_8888)
            return;

        if (AndroidBitmap_lockPixels(env, bitmap, &pixels)
            != ANDROID_BITMAP_RESULT_SUCCESS)
            pixels = nullptr;
    }

    ~LockedPixels()
    {
        if (pixels != nullptr)
            AndroidBitmap_unlockPixels(env, bitmap);
    }

    LockedPixels(const LockedPixels&) = delete;
    LockedPixels& operator=(const LockedPixels&) = delete;

    bool isLocked() const { return pixels != nullptr; }
    bool isStraightAlpha() const
    {
        return (info.flags & ANDROID_BITMAP_FLAGS_ALPHA_MASK)
               != ANDROID_BITMAP_FLAGS_ALPHA_PREMUL;
    }

    std::uint8_t* row(int index) const
    {
        return static_cast<std::uint8_t*>(pixels)
               + (std::size_t) index * info.stride;
    }

    AndroidBitmapInfo info {};

private:
    JNIEnv* env;
    jobject bitmap;
    void* pixels = nullptr;
};

jobject makeDecodeOptions(JNIEnv* env, const AndroidImageJava& java)
{
    auto* options = env->NewObject(java.options, java.optionsInit);

    if (options == nullptr || env->ExceptionCheck())
        return nullptr;

    env->SetObjectField(options, java.inPreferredConfig, java.argb8888);
    env->SetBooleanField(options, java.inPremultiplied, JNI_FALSE);
    env->SetBooleanField(options, java.inScaled, JNI_FALSE);

    return options;
}

bool hasValidSize(int width, int height)
{
    constexpr auto maxPixels = std::numeric_limits<int>::max() / 4;
    return width > 0 && height > 0 && height <= maxPixels / width;
}

Image copyOut(JNIEnv* env, jobject bitmap, std::string& error)
{
    auto locked = LockedPixels {env, bitmap};

    if (!locked.isLocked())
    {
        error = "decoded bitmap is not readable as RGBA_8888";
        return {};
    }

    if (!locked.isStraightAlpha())
    {
        error = "decoded bitmap came back premultiplied";
        return {};
    }

    auto width = (int) locked.info.width;
    auto height = (int) locked.info.height;

    if (!hasValidSize(width, height))
    {
        error = "image has zero dimensions or is too large";
        return {};
    }

    auto image = Image {};
    auto* destination = image.prepareForOverwrite(width, height);
    auto rowBytes = (std::size_t) width * 4;

    for (auto y = 0; y < height; ++y)
        std::memcpy(destination + y * rowBytes, locked.row(y), rowBytes);

    return image;
}

jobject makeBitmapFrom(JNIEnv* env,
                       const AndroidImageJava& java,
                       const std::uint8_t* rgba,
                       int width,
                       int height,
                       bool keepsAlpha)
{
    auto* bitmap = env->CallStaticObjectMethod(
        java.bitmap, java.createBitmap, (jint) width, (jint) height, java.argb8888);

    if (bitmap == nullptr || env->ExceptionCheck())
        return nullptr;

    env->CallVoidMethod(bitmap, java.setHasAlpha, keepsAlpha ? JNI_TRUE : JNI_FALSE);
    env->CallVoidMethod(bitmap, java.setPremultiplied, JNI_FALSE);

    if (env->ExceptionCheck())
        return nullptr;

    auto locked = LockedPixels {env, bitmap};

    if (!locked.isLocked())
        return nullptr;

    auto rowBytes = (std::size_t) width * 4;

    for (auto y = 0; y < height; ++y)
        std::memcpy(locked.row(y), rgba + y * rowBytes, rowBytes);

    return bitmap;
}

jint qualityPercent(float quality)
{
    return (jint) std::lround(std::clamp(quality, 0.f, 1.f) * 100.f);
}

ImageData bytesOf(JNIEnv* env, jbyteArray array)
{
    auto length = env->GetArrayLength(array);
    auto result = ImageData(length);
    env->GetByteArrayRegion(
        array, 0, length, reinterpret_cast<jbyte*>(result.data()));

    return result;
}
} // namespace

Image decodeImageBytes(const std::uint8_t* data, int size, std::string& error)
{
    if (data == nullptr || size <= 0)
    {
        error = "empty image data";
        return {};
    }

    auto session = openSession(error);

    if (session.java == nullptr)
        return {};

    auto* env = session.env;
    const auto& java = *session.java;
    auto frame = Jni::LocalFrame {env};

    auto* bytes = Jni::toJavaBytes(
        env,
        std::string_view {reinterpret_cast<const char*>(data), (std::size_t) size});

    if (bytes == nullptr || env->ExceptionCheck())
    {
        error = javaError(env, "could not copy image bytes to Java");
        return {};
    }

    auto* options = makeDecodeOptions(env, java);

    if (options == nullptr)
    {
        error = javaError(env, "could not build BitmapFactory.Options");
        return {};
    }

    auto* bitmap = env->CallStaticObjectMethod(
        java.bitmapFactory, java.decodeByteArray, bytes, 0, (jint) size, options);

    if (env->ExceptionCheck())
    {
        error = javaError(env, "could not decode image");
        return {};
    }

    if (bitmap == nullptr)
    {
        error = "unrecognized or malformed image data";
        return {};
    }

    auto image = copyOut(env, bitmap, error);

    env->CallVoidMethod(bitmap, java.recycle);
    Jni::failed(env);

    return image;
}

ImageData encodeImageBytes(const std::uint8_t* rgba,
                           int width,
                           int height,
                           ImageFormat format,
                           float quality,
                           std::string& error)
{
    auto session = openSession(error);

    if (session.java == nullptr)
        return {};

    auto* env = session.env;
    const auto& java = *session.java;
    auto frame = Jni::LocalFrame {env};

    auto* bitmap =
        makeBitmapFrom(env, java, rgba, width, height, format == ImageFormat::png);

    if (bitmap == nullptr)
    {
        error = javaError(env, "could not build a bitmap from the pixels");
        return {};
    }

    auto* stream =
        env->NewObject(java.byteStream, java.byteStreamInit, initialEncodeCapacity);
    auto compressed = JNI_FALSE;

    if (stream != nullptr && !env->ExceptionCheck())
    {
        auto* compressFormat = format == ImageFormat::png ? java.png : java.jpeg;
        compressed = env->CallBooleanMethod(
            bitmap, java.compress, compressFormat, qualityPercent(quality), stream);
    }

    auto encoded = ImageData {};

    if (env->ExceptionCheck())
        error = javaError(env, "image encoding failed");
    else if (compressed != JNI_TRUE)
        error = "image encoding failed";
    else
    {
        auto* array =
            static_cast<jbyteArray>(env->CallObjectMethod(stream, java.toByteArray));

        if (array == nullptr || env->ExceptionCheck())
            error = javaError(env, "could not read the encoded bytes");
        else
            encoded = bytesOf(env, array);
    }

    env->CallVoidMethod(bitmap, java.recycle);
    Jni::failed(env);

    return encoded;
}
} // namespace eacp::Graphics::detail

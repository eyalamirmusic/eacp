#include "Clipboard.h"

#include "../Android/Jni.h"
#include "../Utils/Logging.h"

namespace eacp::Clipboard
{
namespace
{
struct JavaClipboard
{
    void resolve(Jni::Lookup& java)
    {
        context = java.findClass("android/content/Context");
        manager = java.findClass("android/content/ClipboardManager");
        clipData = java.findClass("android/content/ClipData");
        clipItem = java.findClass("android/content/ClipData$Item");
        description = java.findClass("android/content/ClipDescription");
        charSequence = java.findClass("java/lang/CharSequence");

        serviceName =
            java.staticObject(context, "CLIPBOARD_SERVICE", "Ljava/lang/String;");
        getSystemService = java.method(
            context, "getSystemService", "(Ljava/lang/String;)Ljava/lang/Object;");

        setPrimaryClip =
            java.method(manager, "setPrimaryClip", "(Landroid/content/ClipData;)V");
        getPrimaryClip =
            java.method(manager, "getPrimaryClip", "()Landroid/content/ClipData;");
        getPrimaryClipDescription =
            java.method(manager,
                        "getPrimaryClipDescription",
                        "()Landroid/content/ClipDescription;");

        newPlainText =
            java.staticMethod(clipData,
                              "newPlainText",
                              "(Ljava/lang/CharSequence;Ljava/lang/CharSequence;)"
                              "Landroid/content/ClipData;");
        getItemCount = java.method(clipData, "getItemCount", "()I");
        getItemAt =
            java.method(clipData, "getItemAt", "(I)Landroid/content/ClipData$Item;");
        getText = java.method(clipItem, "getText", "()Ljava/lang/CharSequence;");
        hasMimeType =
            java.method(description, "hasMimeType", "(Ljava/lang/String;)Z");
        toString = java.method(charSequence, "toString", "()Ljava/lang/String;");
    }

    jclass context = nullptr;
    jclass manager = nullptr;
    jclass clipData = nullptr;
    jclass clipItem = nullptr;
    jclass description = nullptr;
    jclass charSequence = nullptr;

    jobject serviceName = nullptr;
    jmethodID getSystemService = nullptr;
    jmethodID setPrimaryClip = nullptr;
    jmethodID getPrimaryClip = nullptr;
    jmethodID getPrimaryClipDescription = nullptr;
    jmethodID newPlainText = nullptr;
    jmethodID getItemCount = nullptr;
    jmethodID getItemAt = nullptr;
    jmethodID getText = nullptr;
    jmethodID hasMimeType = nullptr;
    jmethodID toString = nullptr;
};

struct Session
{
    JNIEnv* env = nullptr;
    const JavaClipboard* java = nullptr;
    jobject manager = nullptr;

    bool isValid() const { return manager != nullptr; }
};

Session openClipboard(JNIEnv* env)
{
    if (env == nullptr)
        return {};

    const auto* java = Jni::resolveOnce<JavaClipboard>(env);
    auto* context = Jni::applicationContext(env);

    if (java == nullptr || context == nullptr)
        return {};

    auto* manager =
        env->CallObjectMethod(context, java->getSystemService, java->serviceName);

    if (Jni::failed(env))
        return {};

    return {env, java, manager};
}

std::string firstItemText(const Session& session)
{
    auto* env = session.env;
    const auto& java = *session.java;
    auto* clip = env->CallObjectMethod(session.manager, java.getPrimaryClip);

    if (Jni::failed(env) || clip == nullptr)
        return {};

    if (env->CallIntMethod(clip, java.getItemCount) <= 0 || Jni::failed(env))
        return {};

    auto* item = env->CallObjectMethod(clip, java.getItemAt, jint {0});
    auto* text = item != nullptr && !Jni::failed(env)
                     ? env->CallObjectMethod(item, java.getText)
                     : nullptr;

    if (Jni::failed(env) || text == nullptr)
        return {};

    auto* string = env->CallObjectMethod(text, java.toString);
    return Jni::failed(env) ? std::string {} : Jni::toString(env, string);
}
} // namespace

bool copyText(std::string_view text)
{
    auto* env = Jni::currentEnv();

    if (env == nullptr)
        return false;

    auto frame = Jni::LocalFrame {env};
    auto session = openClipboard(env);

    if (!session.isValid())
        return false;

    const auto& java = *session.java;
    auto* label = Jni::toJava(env, std::string_view {});
    auto* content = Jni::toJava(env, text);
    auto* clip = env->CallStaticObjectMethod(
        java.clipData, java.newPlainText, label, content);

    if (Jni::failed(env) || clip == nullptr)
        return false;

    env->CallVoidMethod(session.manager, java.setPrimaryClip, clip);
    return !Jni::failed(env);
}

// From API 29 a read returns nothing unless the app has input focus.
std::string getText()
{
    auto* env = Jni::currentEnv();

    if (env == nullptr)
        return {};

    auto frame = Jni::LocalFrame {env};
    auto session = openClipboard(env);

    return session.isValid() ? firstItemText(session) : std::string {};
}

bool hasText()
{
    auto* env = Jni::currentEnv();

    if (env == nullptr)
        return false;

    auto frame = Jni::LocalFrame {env};
    auto session = openClipboard(env);

    if (!session.isValid())
        return false;

    const auto& java = *session.java;
    auto* description =
        env->CallObjectMethod(session.manager, java.getPrimaryClipDescription);

    if (Jni::failed(env) || description == nullptr)
        return false;

    auto* anyText = Jni::toJava(env, std::string_view {"text/*"});
    auto result = env->CallBooleanMethod(description, java.hasMimeType, anyText);

    return !Jni::failed(env) && result == JNI_TRUE;
}

bool copyFiles(const Vector<std::string>&)
{
    LOG("Clipboard: copying files is not supported on Android, which shares "
        "files as content:// URIs rather than paths");
    return false;
}
} // namespace eacp::Clipboard

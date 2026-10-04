#include "App.h"

#include "../Android/Jni.h"
#include "../Utils/Logging.h"

namespace eacp::Apps
{
namespace
{
constexpr auto flagActivityNewTask = jint {0x10000000};

struct JavaIntent
{
    void resolve(Jni::Lookup& java)
    {
        context = java.findClass("android/content/Context");
        intent = java.findClass("android/content/Intent");
        uri = java.findClass("android/net/Uri");

        actionView = java.staticObject(intent, "ACTION_VIEW", "Ljava/lang/String;");
        intentInit =
            java.method(intent, "<init>", "(Ljava/lang/String;Landroid/net/Uri;)V");
        addFlags = java.method(intent, "addFlags", "(I)Landroid/content/Intent;");
        parse =
            java.staticMethod(uri, "parse", "(Ljava/lang/String;)Landroid/net/Uri;");
        startActivity =
            java.method(context, "startActivity", "(Landroid/content/Intent;)V");
    }

    jclass context = nullptr;
    jclass intent = nullptr;
    jclass uri = nullptr;

    jobject actionView = nullptr;
    jmethodID intentInit = nullptr;
    jmethodID addFlags = nullptr;
    jmethodID parse = nullptr;
    jmethodID startActivity = nullptr;
};

void logFailure(JNIEnv* env, const std::string& url)
{
    if (auto exception = Jni::takeException(env); !exception.empty())
        LOG("openExternalURL: ", url, ": ", exception);
}

void viewURL(JNIEnv* env, const std::string& url)
{
    const auto* java = Jni::resolveOnce<JavaIntent>(env);
    auto* context = Jni::applicationContext(env);

    if (java == nullptr || context == nullptr)
    {
        LOG("openExternalURL: no Context to start an activity from");
        return;
    }

    auto* uri = env->CallStaticObjectMethod(
        java->uri, java->parse, Jni::toJava(env, std::string_view {url}));

    if (env->ExceptionCheck() || uri == nullptr)
        return logFailure(env, url);

    auto* intent =
        env->NewObject(java->intent, java->intentInit, java->actionView, uri);

    if (env->ExceptionCheck() || intent == nullptr)
        return logFailure(env, url);

    env->DeleteLocalRef(
        env->CallObjectMethod(intent, java->addFlags, flagActivityNewTask));

    if (!env->ExceptionCheck())
        env->CallVoidMethod(context, java->startActivity, intent);

    logFailure(env, url);
}

std::nullopt_t unsupportedPicker(std::string_view name)
{
    LOG(name, ": not supported on Android, which picks files through an activity");
    return std::nullopt;
}
} // namespace

void setDockIconVisible(bool) {}

void setAppBadge(const std::string&) {}

bool isSystemPoweringOff()
{
    return false;
}

void Detail::observeSystemPowerOff() {}

bool isDistributionSigned()
{
    return false;
}

void openExternalURL(const std::string& url)
{
    auto* env = Jni::currentEnv();

    if (env == nullptr)
    {
        LOG("openExternalURL: no JavaVM; nothing called Jni::setJavaVM");
        return;
    }

    auto frame = Jni::LocalFrame {env};
    viewURL(env, url);
}

std::optional<std::string> chooseFile(const FilePickerOptions&)
{
    return unsupportedPicker("chooseFile");
}

std::optional<std::string> chooseSaveFile(const FileSaveOptions&)
{
    return unsupportedPicker("chooseSaveFile");
}

std::optional<std::string> chooseDirectory()
{
    return unsupportedPicker("chooseDirectory");
}
} // namespace eacp::Apps

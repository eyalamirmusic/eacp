#include "AndroidEnvironment-Android.h"

#include <eacp/Core/Android/Jni.h>
#include <eacp/Core/Utils/Environment.h>
#include <eacp/Core/Utils/Logging.h>

#include <android/native_activity.h>
#include <sys/system_properties.h>

#include <sstream>
#include <string>

namespace eacp::Graphics
{
namespace
{
constexpr jint localReferencesPerExtra = 8;

void importVariable(const std::string& name,
                    const std::string& value,
                    std::string_view from)
{
    if (name.empty())
        return;

    setEnv(name, value);
    LOG("Android: ", name, "=", value, " from ", from);
}

std::string systemProperty(const std::string& name)
{
    auto value = std::string {};
    const auto* info = __system_property_find(name.c_str());

    if (info == nullptr)
        return value;

    __system_property_read_callback(
        info,
        [](void* cookie, const char*, const char* text, uint32_t)
        { *static_cast<std::string*>(cookie) = text; },
        &value);

    return value;
}

void importSystemProperty(const std::string& package)
{
    const auto name = "debug." + package + ".env";
    auto settings = std::istringstream {systemProperty(name)};
    auto setting = std::string {};

    while (settings >> setting)
    {
        const auto equals = setting.find('=');

        if (equals != std::string::npos)
            importVariable(
                setting.substr(0, equals), setting.substr(equals + 1), name);
    }
}

void importExtra(JNIEnv* env, jobject extras, jmethodID getString, jobject key)
{
    auto frame = Jni::LocalFrame {env, localReferencesPerExtra};
    auto* value = env->CallObjectMethod(extras, getString, key);

    if (!Jni::failed(env) && value != nullptr)
        importVariable(
            Jni::toString(env, key), Jni::toString(env, value), "the launch intent");
}

void importIntentExtras(JNIEnv* env, jobject activity)
{
    auto* intent =
        Jni::callObject(env, activity, "getIntent", "()Landroid/content/Intent;");
    auto* extras =
        Jni::callObject(env, intent, "getExtras", "()Landroid/os/Bundle;");
    auto* keySet = Jni::callObject(env, extras, "keySet", "()Ljava/util/Set;");
    auto* keys = static_cast<jobjectArray>(
        Jni::callObject(env, keySet, "toArray", "()[Ljava/lang/Object;"));

    if (keys == nullptr)
        return;

    auto getString = env->GetMethodID(env->GetObjectClass(extras),
                                      "getString",
                                      "(Ljava/lang/String;)Ljava/lang/String;");

    if (Jni::failed(env))
        return;

    const auto count = env->GetArrayLength(keys);

    for (auto index = jsize {0}; index < count; ++index)
    {
        auto* key = env->GetObjectArrayElement(keys, index);

        if (!Jni::failed(env))
            importExtra(env, extras, getString, key);

        env->DeleteLocalRef(key);
    }
}

void importEnvironment(JNIEnv* env, jobject activity)
{
    auto* package =
        Jni::callObject(env, activity, "getPackageName", "()Ljava/lang/String;");

    if (package != nullptr)
        importSystemProperty(Jni::toString(env, package));

    importIntentExtras(env, activity);
}
} // namespace

void importAndroidEnvironment([[maybe_unused]] ANativeActivity* activity)
{
#ifndef NDEBUG
    auto* env = Jni::currentEnv();

    if (env == nullptr || activity == nullptr)
        return;

    auto frame = Jni::LocalFrame {env};
    importEnvironment(env, activity->clazz);
#endif
}
} // namespace eacp::Graphics

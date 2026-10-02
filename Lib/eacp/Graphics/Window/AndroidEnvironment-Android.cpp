#include "AndroidEnvironment-Android.h"

#include <eacp/Core/Utils/Environment.h>
#include <eacp/Core/Utils/Logging.h>

#include <android/native_activity.h>
#include <jni.h>
#include <sys/system_properties.h>

#include <sstream>
#include <string>

namespace eacp::Graphics
{
namespace
{
constexpr jint localReferencesPerExtra = 8;

bool javaFailed(JNIEnv* env)
{
    if (!env->ExceptionCheck())
        return false;

    env->ExceptionClear();
    return true;
}

jobject
    callObject(JNIEnv* env, jobject target, const char* name, const char* signature)
{
    if (target == nullptr)
        return nullptr;

    auto method = env->GetMethodID(env->GetObjectClass(target), name, signature);

    if (javaFailed(env))
        return nullptr;

    auto* result = env->CallObjectMethod(target, method);
    return javaFailed(env) ? nullptr : result;
}

std::string javaString(JNIEnv* env, jobject text)
{
    if (text == nullptr)
        return {};

    auto* chars = env->GetStringUTFChars(static_cast<jstring>(text), nullptr);

    if (chars == nullptr)
    {
        javaFailed(env);
        return {};
    }

    auto result = std::string {chars};
    env->ReleaseStringUTFChars(static_cast<jstring>(text), chars);

    return result;
}

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
    if (env->PushLocalFrame(localReferencesPerExtra) != 0)
    {
        javaFailed(env);
        return;
    }

    auto* value = env->CallObjectMethod(extras, getString, key);

    if (!javaFailed(env) && value != nullptr)
        importVariable(
            javaString(env, key), javaString(env, value), "the launch intent");

    env->PopLocalFrame(nullptr);
}

void importIntentExtras(JNIEnv* env, jobject activity)
{
    auto* intent =
        callObject(env, activity, "getIntent", "()Landroid/content/Intent;");
    auto* extras = callObject(env, intent, "getExtras", "()Landroid/os/Bundle;");
    auto* keySet = callObject(env, extras, "keySet", "()Ljava/util/Set;");
    auto* keys = static_cast<jobjectArray>(
        callObject(env, keySet, "toArray", "()[Ljava/lang/Object;"));

    if (keys == nullptr)
        return;

    auto getString = env->GetMethodID(env->GetObjectClass(extras),
                                      "getString",
                                      "(Ljava/lang/String;)Ljava/lang/String;");

    if (javaFailed(env))
        return;

    const auto count = env->GetArrayLength(keys);

    for (auto index = jsize {0}; index < count; ++index)
    {
        auto* key = env->GetObjectArrayElement(keys, index);

        if (!javaFailed(env))
            importExtra(env, extras, getString, key);

        env->DeleteLocalRef(key);
    }
}

void importEnvironment(JNIEnv* env, jobject activity)
{
    auto* package =
        callObject(env, activity, "getPackageName", "()Ljava/lang/String;");

    if (package != nullptr)
        importSystemProperty(javaString(env, package));

    importIntentExtras(env, activity);
}
} // namespace

void importAndroidEnvironment(ANativeActivity* activity)
{
    if (activity == nullptr || activity->vm == nullptr)
        return;

    auto* env = static_cast<JNIEnv*>(nullptr);
    auto attached = false;

    if (activity->vm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6)
        != JNI_OK)
    {
        if (activity->vm->AttachCurrentThread(&env, nullptr) != JNI_OK)
        {
            LOG("Android: could not attach to the VM; no environment imported");
            return;
        }

        attached = true;
    }

    importEnvironment(env, activity->clazz);

    if (attached)
        activity->vm->DetachCurrentThread();
}
} // namespace eacp::Graphics

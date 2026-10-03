#include "Jni.h"

#include <eacp/Core/Utils/Logging.h>

namespace eacp::Jni
{
namespace
{
JavaVM* javaVM = nullptr;

struct ThreadAttachment final
{
    ~ThreadAttachment()
    {
        if (vm != nullptr)
            vm->DetachCurrentThread();
    }

    JavaVM* vm = nullptr;
};

template <typename Id>
Id found(Lookup& lookup, Id id, const char* name, const char* signature)
{
    if (failed(lookup.env) || id == nullptr)
    {
        LOG("JNI: no ", name, " ", signature);
        lookup.complete = false;
        return nullptr;
    }

    return id;
}
} // namespace

void setJavaVM(JavaVM* vm)
{
    javaVM = vm;
}

JNIEnv* currentEnv()
{
    auto* env = static_cast<JNIEnv*>(nullptr);

    if (javaVM == nullptr)
        return nullptr;

    if (javaVM->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6) == JNI_OK)
        return env;

    if (javaVM->AttachCurrentThread(&env, nullptr) != JNI_OK)
    {
        LOG("JNI: could not attach this thread to the VM");
        return nullptr;
    }

    thread_local auto attachment = ThreadAttachment {};
    attachment.vm = javaVM;

    return env;
}

bool failed(JNIEnv* env)
{
    if (!env->ExceptionCheck())
        return false;

    env->ExceptionDescribe();
    env->ExceptionClear();
    return true;
}

LocalFrame::LocalFrame(JNIEnv* envToUse, jint capacity)
    : env(envToUse)
    , pushed(env->PushLocalFrame(capacity) == 0)
{
    if (!pushed)
        failed(env);
}

LocalFrame::~LocalFrame()
{
    if (pushed)
        env->PopLocalFrame(nullptr);
}

std::string toString(JNIEnv* env, jobject text)
{
    if (text == nullptr)
        return {};

    auto* chars = env->GetStringUTFChars(static_cast<jstring>(text), nullptr);

    if (chars == nullptr)
    {
        failed(env);
        return {};
    }

    auto result = std::string {chars};
    env->ReleaseStringUTFChars(static_cast<jstring>(text), chars);

    return result;
}

jstring toJava(JNIEnv* env, std::u16string_view text)
{
    return env->NewString(reinterpret_cast<const jchar*>(text.data()),
                          (jsize) text.size());
}

jobject
    callObject(JNIEnv* env, jobject target, const char* name, const char* signature)
{
    if (target == nullptr)
        return nullptr;

    auto method = env->GetMethodID(env->GetObjectClass(target), name, signature);

    if (failed(env))
        return nullptr;

    auto* result = env->CallObjectMethod(target, method);
    return failed(env) ? nullptr : result;
}

jclass Lookup::findClass(const char* name)
{
    auto* local = found(*this, env->FindClass(name), name, "");
    return local != nullptr ? static_cast<jclass>(env->NewGlobalRef(local))
                            : nullptr;
}

jmethodID Lookup::method(jclass owner, const char* name, const char* signature)
{
    auto id = owner != nullptr ? env->GetMethodID(owner, name, signature) : nullptr;
    return found(*this, id, name, signature);
}

jmethodID Lookup::staticMethod(jclass owner, const char* name, const char* signature)
{
    auto id =
        owner != nullptr ? env->GetStaticMethodID(owner, name, signature) : nullptr;
    return found(*this, id, name, signature);
}

jfieldID Lookup::field(jclass owner, const char* name, const char* signature)
{
    auto id = owner != nullptr ? env->GetFieldID(owner, name, signature) : nullptr;
    return found(*this, id, name, signature);
}

jobject Lookup::staticObject(jclass owner, const char* name, const char* signature)
{
    auto id =
        owner != nullptr ? env->GetStaticFieldID(owner, name, signature) : nullptr;
    auto* local = id != nullptr && !failed(env)
                      ? env->GetStaticObjectField(owner, id)
                      : nullptr;
    local = found(*this, local, name, signature);

    return local != nullptr ? env->NewGlobalRef(local) : nullptr;
}
} // namespace eacp::Jni

#pragma once

#include <jni.h>

#include <string>
#include <string_view>

// The few calls eacp makes into the framework's Java classes, from native
// threads that never return to Java.
namespace eacp::Jni
{
void setJavaVM(JavaVM* vm);

// The calling thread's env, attached on first use and detached when the thread
// ends. Null before setJavaVM, or when the attach fails.
JNIEnv* currentEnv();

// Logs and clears a pending exception; true when there was one.
bool failed(JNIEnv* env);

// Every local reference made while it lives is released when it goes, since
// nothing else would release them on a thread that never returns to Java.
struct LocalFrame final
{
    explicit LocalFrame(JNIEnv* envToUse, jint capacity = 32);
    ~LocalFrame();

    LocalFrame(const LocalFrame&) = delete;
    LocalFrame& operator=(const LocalFrame&) = delete;

    JNIEnv* env;
    bool pushed;
};

std::string toString(JNIEnv* env, jobject text);
jstring toJava(JNIEnv* env, std::u16string_view text);

// target.name() for a method with no arguments that returns an object; null on
// any failure, a null target included.
jobject
    callObject(JNIEnv* env, jobject target, const char* name, const char* signature);

// Looks classes, ids and constants up by name, logging each one that is missing.
// Classes and constants come back as global references, kept for the process.
struct Lookup final
{
    jclass findClass(const char* name);
    jmethodID method(jclass owner, const char* name, const char* signature);
    jmethodID staticMethod(jclass owner, const char* name, const char* signature);
    jfieldID field(jclass owner, const char* name, const char* signature);
    jobject staticObject(jclass owner, const char* name, const char* signature);

    JNIEnv* env;
    bool complete = true;
};

// The table T, filled once for the process by T::resolve(Lookup&); null when
// anything in it was missing.
template <typename T>
const T* resolveOnce(JNIEnv* env)
{
    static const auto* resolved = [env]() -> const T*
    {
        static auto table = T {};
        auto frame = LocalFrame {env};
        auto lookup = Lookup {env};
        table.resolve(lookup);

        return lookup.complete ? &table : nullptr;
    }();

    return resolved;
}
} // namespace eacp::Jni

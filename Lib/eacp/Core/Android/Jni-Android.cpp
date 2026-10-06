#include "Jni.h"

#include <eacp/Core/Utils/Logging.h>

#include <atomic>
#include <mutex>

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

int sequenceLength(unsigned char lead)
{
    if (lead < 0x80)
        return 1;

    if ((lead & 0xE0) == 0xC0)
        return 2;

    if ((lead & 0xF0) == 0xE0)
        return 3;

    if ((lead & 0xF8) == 0xF0)
        return 4;

    return 0;
}

char32_t decodeOne(std::string_view utf8, std::size_t& index)
{
    constexpr auto replacement = char32_t {0xFFFD};
    auto lead = (unsigned char) utf8[index++];
    auto length = sequenceLength(lead);

    if (length == 1)
        return lead;

    if (length == 0 || index + (std::size_t) length - 1 > utf8.size())
        return replacement;

    auto codepoint = (char32_t) (lead & (0x7F >> length));

    for (auto i = 1; i < length; ++i)
    {
        auto next = (unsigned char) utf8[index];

        if ((next & 0xC0) != 0x80)
            return replacement;

        codepoint = (codepoint << 6) | (next & 0x3F);
        ++index;
    }

    return codepoint > 0x10FFFF ? replacement : codepoint;
}

std::u16string utf16From(std::string_view utf8)
{
    auto result = std::u16string {};
    result.reserve(utf8.size());

    for (auto index = std::size_t {0}; index < utf8.size();)
    {
        auto codepoint = decodeOne(utf8, index);

        if (codepoint < 0x10000)
        {
            result.push_back((char16_t) codepoint);
            continue;
        }

        codepoint -= 0x10000;
        result.push_back((char16_t) (0xD800 + (codepoint >> 10)));
        result.push_back((char16_t) (0xDC00 + (codepoint & 0x3FF)));
    }

    return result;
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

namespace
{
std::atomic<jobject>& storedContext()
{
    static auto context = std::atomic<jobject> {nullptr};
    return context;
}

jobject keepForTheProcess(JNIEnv* env, jobject local)
{
    return local != nullptr ? env->NewGlobalRef(local) : nullptr;
}

jobject currentApplication(JNIEnv* env)
{
    auto frame = LocalFrame {env};
    auto* activityThread = env->FindClass("android/app/ActivityThread");

    if (failed(env) || activityThread == nullptr)
        return nullptr;

    auto method = env->GetStaticMethodID(
        activityThread, "currentApplication", "()Landroid/app/Application;");

    if (failed(env) || method == nullptr)
        return nullptr;

    auto* application = env->CallStaticObjectMethod(activityThread, method);
    return failed(env) ? nullptr : keepForTheProcess(env, application);
}
} // namespace

void setContext(JNIEnv* env, jobject anyContext)
{
    if (env == nullptr || anyContext == nullptr)
        return;

    auto frame = LocalFrame {env};
    auto* application = callObject(
        env, anyContext, "getApplicationContext", "()Landroid/content/Context;");
    auto* kept =
        keepForTheProcess(env, application != nullptr ? application : anyContext);

    if (auto* previous = storedContext().exchange(kept))
        env->DeleteGlobalRef(previous);
}

jobject applicationContext(JNIEnv* env)
{
    if (auto* context = storedContext().load())
        return context;

    if (env == nullptr)
        return nullptr;

    auto* found = currentApplication(env);
    auto* expected = static_cast<jobject>(nullptr);

    if (found == nullptr)
    {
        LOG("JNI: no application Context; nothing called Jni::setContext");
        return nullptr;
    }

    if (storedContext().compare_exchange_strong(expected, found))
        return found;

    env->DeleteGlobalRef(found);
    return expected;
}

namespace
{
struct StoredActivity final
{
    std::mutex lock;
    jobject activity = nullptr;
};

StoredActivity& storedActivity()
{
    static auto stored = StoredActivity {};
    return stored;
}
} // namespace

void setActivity(JNIEnv* env, jobject activity)
{
    if (env == nullptr)
        return;

    auto& stored = storedActivity();
    auto guard = std::scoped_lock {stored.lock};

    if (stored.activity != nullptr)
        env->DeleteGlobalRef(stored.activity);

    stored.activity = keepForTheProcess(env, activity);
}

jobject activity(JNIEnv* env)
{
    if (env == nullptr)
        return nullptr;

    auto& stored = storedActivity();
    auto guard = std::scoped_lock {stored.lock};

    return stored.activity != nullptr ? env->NewLocalRef(stored.activity) : nullptr;
}

bool failed(JNIEnv* env)
{
    if (!env->ExceptionCheck())
        return false;

    env->ExceptionDescribe();
    env->ExceptionClear();
    return true;
}

std::string takeException(JNIEnv* env)
{
    if (!env->ExceptionCheck())
        return {};

    auto* throwable = env->ExceptionOccurred();
    env->ExceptionClear();

    auto* text = callObject(env, throwable, "toString", "()Ljava/lang/String;");
    auto message = toString(env, text);

    env->DeleteLocalRef(text);
    env->DeleteLocalRef(throwable);

    return message.empty() ? std::string {"Java exception"} : message;
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

GlobalRef::~GlobalRef()
{
    if (object == nullptr)
        return;

    if (auto* env = currentEnv())
        env->DeleteGlobalRef(object);
}

void GlobalRef::reset(JNIEnv* env, jobject local)
{
    if (object != nullptr)
        env->DeleteGlobalRef(object);

    object = local != nullptr ? env->NewGlobalRef(local) : nullptr;
}

jobject GlobalRef::get() const
{
    return object;
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

jstring toJava(JNIEnv* env, std::string_view utf8)
{
    return toJava(env, std::u16string_view {utf16From(utf8)});
}

jbyteArray toJavaBytes(JNIEnv* env, std::string_view bytes)
{
    auto* array = env->NewByteArray((jsize) bytes.size());

    if (array == nullptr)
        return nullptr;

    env->SetByteArrayRegion(array,
                            0,
                            (jsize) bytes.size(),
                            reinterpret_cast<const jbyte*>(bytes.data()));

    return array;
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

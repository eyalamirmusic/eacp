#include "Permissions-Android.h"
#include "Jni.h"

#include <eacp/Core/Threads/EventLoop.h>
#include <eacp/Core/Utils/Containers.h>
#include <eacp/Core/Utils/Logging.h>

#include <mutex>
#include <string>
#include <string_view>

namespace eacp::Android
{
namespace
{
using ResultCallback = std::function<void(bool)>;

constexpr auto permissionGranted = jint {0};
constexpr auto permissionRequestCode = jint {0xEAC};

struct PermissionsJava
{
    void resolve(Jni::Lookup& java)
    {
        context = java.findClass("android/content/Context");
        activity = java.findClass("android/app/Activity");
        string = java.findClass("java/lang/String");

        checkSelfPermission =
            java.method(context, "checkSelfPermission", "(Ljava/lang/String;)I");
        requestPermissions =
            java.method(activity, "requestPermissions", "([Ljava/lang/String;I)V");
    }

    jclass context = nullptr;
    jclass activity = nullptr;
    jclass string = nullptr;

    jmethodID checkSelfPermission = nullptr;
    jmethodID requestPermissions = nullptr;
};

struct PendingRequest
{
    std::string permission;
    Vector<ResultCallback> callbacks;
    bool issued = false;
    bool pausedSinceIssued = false;
};

struct PendingRequests
{
    std::mutex lock;
    Vector<PendingRequest> requests;
    bool paused = false;
};

PendingRequests& pendingRequests()
{
    static auto pending = PendingRequests {};
    return pending;
}

void deliver(const Vector<ResultCallback>& callbacks, bool granted)
{
    for (const auto& callback: callbacks)
    {
        auto answer = [callback, granted] { callback(granted); };
        Threads::callAsync(answer);
    }
}

bool askActivity(const Vector<std::string>& permissions)
{
    auto* env = Jni::currentEnv();

    if (env == nullptr)
        return false;

    auto frame = Jni::LocalFrame {env, permissions.size() + 8};
    const auto* java = Jni::resolveOnce<PermissionsJava>(env);
    auto* activity = Jni::activity(env);

    if (java == nullptr || activity == nullptr)
    {
        LOG("requestPermission: no activity to ask from");
        return false;
    }

    auto* array = env->NewObjectArray(permissions.size(), java->string, nullptr);

    if (Jni::failed(env) || array == nullptr)
        return false;

    for (auto index = 0; index < permissions.size(); ++index)
    {
        auto* name = Jni::toJava(env, std::string_view {permissions[index]});

        if (Jni::failed(env) || name == nullptr)
            return false;

        env->SetObjectArrayElement(array, index, name);

        if (Jni::failed(env))
            return false;
    }

    env->CallVoidMethod(
        activity, java->requestPermissions, array, permissionRequestCode);

    return !Jni::failed(env);
}

void refuse(const Vector<std::string>& permissions)
{
    auto refused = Vector<ResultCallback> {};
    auto isRefused = [&permissions](const PendingRequest& request)
    { return permissions.contains(request.permission); };

    {
        auto& pending = pendingRequests();
        auto guard = std::scoped_lock {pending.lock};

        for (const auto& request: pending.requests)
            if (isRefused(request))
                refused.addFrom(request.callbacks);

        pending.requests.eraseIf(isRefused);
    }

    deliver(refused, false);
}

void issue(const Vector<std::string>& permissions)
{
    if (!permissions.empty() && !askActivity(permissions))
        refuse(permissions);
}

bool anyIssued(const Vector<PendingRequest>& requests)
{
    for (const auto& request: requests)
        if (request.issued)
            return true;

    return false;
}

Vector<std::string> issueWaiting(PendingRequests& pending)
{
    auto permissions = Vector<std::string> {};

    if (anyIssued(pending.requests))
        return permissions;

    for (auto& request: pending.requests)
    {
        request.issued = true;
        request.pausedSinceIssued = pending.paused;
        permissions.add(request.permission);
    }

    return permissions;
}
} // namespace

bool hasPermission(const char* permission)
{
    auto* env = Jni::currentEnv();

    if (env == nullptr || permission == nullptr)
        return false;

    auto frame = Jni::LocalFrame {env};
    const auto* java = Jni::resolveOnce<PermissionsJava>(env);
    auto* context = Jni::applicationContext(env);

    if (java == nullptr || context == nullptr)
        return false;

    auto result =
        env->CallIntMethod(context,
                           java->checkSelfPermission,
                           Jni::toJava(env, std::string_view {permission}));

    return !Jni::failed(env) && result == permissionGranted;
}

void requestPermission(const char* permission,
                       const std::function<void(bool)>& onResult)
{
    if (permission == nullptr || hasPermission(permission))
    {
        deliver({onResult}, permission != nullptr);
        return;
    }

    auto toIssue = Vector<std::string> {};

    {
        auto& pending = pendingRequests();
        auto guard = std::scoped_lock {pending.lock};
        auto matches = [permission](const PendingRequest& request)
        { return request.permission == permission; };

        if (auto* request = pending.requests.findIf(matches))
        {
            request->callbacks.add(onResult);
            return;
        }

        auto& request = pending.requests.create();
        request.permission = permission;
        request.callbacks.add(onResult);
        toIssue = issueWaiting(pending);
    }

    issue(toIssue);
}

void Detail::permissionsActivityPaused()
{
    auto& pending = pendingRequests();
    auto guard = std::scoped_lock {pending.lock};

    pending.paused = true;

    for (auto& request: pending.requests)
        if (request.issued)
            request.pausedSinceIssued = true;
}

void Detail::permissionsActivityResumed()
{
    auto issuedPermissions = Vector<std::string> {};

    {
        auto& pending = pendingRequests();
        auto guard = std::scoped_lock {pending.lock};

        for (const auto& request: pending.requests)
            if (request.issued)
                issuedPermissions.add(request.permission);
    }

    auto granted = Vector<std::string> {};

    for (const auto& permission: issuedPermissions)
        if (hasPermission(permission.c_str()))
            granted.add(permission);

    auto answered = Vector<PendingRequest> {};
    auto toIssue = Vector<std::string> {};
    auto isAnswered = [&granted](const PendingRequest& request)
    {
        return request.issued
               && (request.pausedSinceIssued
                   || granted.contains(request.permission));
    };

    {
        auto& pending = pendingRequests();
        auto guard = std::scoped_lock {pending.lock};

        pending.paused = false;

        for (const auto& request: pending.requests)
            if (isAnswered(request))
                answered.add(request);

        pending.requests.eraseIf(isAnswered);
        toIssue = issueWaiting(pending);
    }

    for (const auto& request: answered)
        deliver(request.callbacks, hasPermission(request.permission.c_str()));

    issue(toIssue);
}
} // namespace eacp::Android

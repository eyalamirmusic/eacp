#include <eacp/Core/Utils/WinInclude.h>

#include "GlobalMouseClickMonitor.h"

#include <eacp/Core/Threads/EventLoop.h>

namespace eacp::Graphics
{
namespace
{
// A low-level hook has no user data, so the live monitors share one hook and
// are listed here. Only the main thread touches these: the hook runs on the
// thread that installed it.
Vector<Callback*>& getLiveCallbacks()
{
    static auto callbacks = Vector<Callback*> {};
    return callbacks;
}

HHOOK& getSharedHook()
{
    static auto hook = HHOOK {nullptr};
    return hook;
}

bool isButtonPress(WPARAM message)
{
    return message == WM_LBUTTONDOWN || message == WM_RBUTTONDOWN
           || message == WM_MBUTTONDOWN || message == WM_XBUTTONDOWN;
}

LRESULT CALLBACK onLowLevelMouse(int code, WPARAM message, LPARAM data)
{
    if (code == HC_ACTION && isButtonPress(message))
    {
        // Deferred: the hook must return quickly or Windows drops it.
        for (auto* callback: getLiveCallbacks())
            Threads::callAsync(*callback);
    }

    return CallNextHookEx(nullptr, code, message, data);
}

void addLiveCallback(Callback& callback)
{
    auto& hook = getSharedHook();

    if (hook == nullptr)
        hook = SetWindowsHookExW(
            WH_MOUSE_LL, onLowLevelMouse, GetModuleHandleW(nullptr), 0);

    getLiveCallbacks().add(&callback);
}

void removeLiveCallback(Callback& callback)
{
    auto& callbacks = getLiveCallbacks();
    callbacks.removeAllMatches(&callback);

    auto& hook = getSharedHook();

    if (callbacks.empty() && hook != nullptr)
    {
        UnhookWindowsHookEx(hook);
        hook = nullptr;
    }
}
} // namespace

struct GlobalMouseClickMonitor::Native
{
    explicit Native(Callback callback)
        : onPressed(std::move(callback))
    {
        addLiveCallback(onPressed);
    }

    ~Native() { removeLiveCallback(onPressed); }

    Callback onPressed;
};

GlobalMouseClickMonitor::GlobalMouseClickMonitor(Callback onPressed)
    : impl(std::move(onPressed))
{
}

GlobalMouseClickMonitor::~GlobalMouseClickMonitor() = default;

} // namespace eacp::Graphics

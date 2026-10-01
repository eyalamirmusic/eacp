#include "KeyGrab.h"
#include "KeyEchoes.h"
#include "KeyForwarding-Windows.h"

#include <cwchar>

namespace eacp::Graphics
{

namespace
{
struct KeyInterceptor
{
    virtual bool intercept(const MSG& msg) = 0;

protected:
    ~KeyInterceptor() = default;
};

Vector<KeyInterceptor*>& activeKeyGrabs()
{
    static auto grabs = Vector<KeyInterceptor*> {};
    return grabs;
}

HHOOK keyGrabHook = nullptr;

LRESULT CALLBACK keyGrabHookProc(int code, WPARAM wParam, LPARAM lParam);

void ensureKeyGrabHook()
{
    if (keyGrabHook == nullptr)
        keyGrabHook = SetWindowsHookExW(
            WH_GETMESSAGE, keyGrabHookProc, nullptr, GetCurrentThreadId());
}

void releaseKeyGrabHookIfUnused()
{
    if (!activeKeyGrabs().empty() || keyGrabHook == nullptr)
        return;

    UnhookWindowsHookEx(keyGrabHook);
    keyGrabHook = nullptr;
}

bool isTextInputClass(const wchar_t* name)
{
    return _wcsicmp(name, L"Edit") == 0 || _wcsnicmp(name, L"RichEdit", 8) == 0;
}

bool isEditingText()
{
    auto focus = GetFocus();

    if (focus == nullptr)
        return false;

    wchar_t name[64] = {};
    GetClassNameW(focus, name, 64);
    return isTextInputClass(name);
}

NativeKeyEvent keyUpFor(uint32_t virtualKey)
{
    return nativeKeyEventFrom(WM_KEYUP,
                              static_cast<WPARAM>(virtualKey),
                              static_cast<LPARAM>(0xC0000001),
                              GetTickCount());
}
} // namespace

struct KeyGrab::Native final : KeyInterceptor
{
    Native(Window& windowToUse, Predicate shouldGrabToUse, Handler onGrabbedToUse)
        : window((HWND) windowToUse.getHandle())
        , shouldGrab(std::move(shouldGrabToUse))
        , onGrabbed(std::move(onGrabbedToUse))
    {
        activeKeyGrabs().add(this);
        ensureKeyGrabHook();
    }

    ~Native()
    {
        activeKeyGrabs().removeAllMatches(this);
        releaseKeyGrabHookIfUnused();
        releaseHeldKeys();
    }

    bool intercept(const MSG& msg) override
    {
        if (!isPlainKeyMessage(msg.message))
            return false;

        auto event =
            nativeKeyEventFrom(msg.message, msg.wParam, msg.lParam, msg.time);

        if (isInWindow(msg.hwnd) && isEchoOfForwardedKey(event))
            return true;

        auto claimed = event.key.type == KeyEventType::Down
                           ? claimDown(msg.hwnd, event)
                           : releaseUp(event);

        if (claimed)
            onGrabbed(event);

        return claimed;
    }

    bool claimDown(HWND target, const NativeKeyEvent& event)
    {
        if (!isInWindow(target) || isEditingText() || !shouldGrab(event.key))
            return false;

        heldKeys.addIfNotThere(event.nativeKey);
        return true;
    }

    bool releaseUp(const NativeKeyEvent& event)
    {
        return heldKeys.removeAllMatches(event.nativeKey) > 0;
    }

    bool isInWindow(HWND target) const
    {
        return target != nullptr && (target == window || IsChild(window, target));
    }

    void releaseHeldKeys()
    {
        for (auto virtualKey: heldKeys)
            onGrabbed(keyUpFor(virtualKey));

        heldKeys.clear();
    }

    HWND window;
    Predicate shouldGrab;
    Handler onGrabbed;
    Vector<uint32_t> heldKeys;
};

namespace
{
LRESULT CALLBACK keyGrabHookProc(int code, WPARAM wParam, LPARAM lParam)
{
    if (code == HC_ACTION && wParam == PM_REMOVE)
    {
        auto* msg = reinterpret_cast<MSG*>(lParam);
        auto grabs = activeKeyGrabs();

        for (auto* grab: grabs)
        {
            if (activeKeyGrabs().contains(grab) && grab->intercept(*msg))
            {
                msg->message = WM_NULL;
                break;
            }
        }
    }

    return CallNextHookEx(nullptr, code, wParam, lParam);
}
} // namespace

KeyGrab::KeyGrab(Window& window, Predicate shouldGrab, Handler onGrabbed)
    : impl(window, std::move(shouldGrab), std::move(onGrabbed))
{
}

KeyGrab::~KeyGrab() = default;

} // namespace eacp::Graphics

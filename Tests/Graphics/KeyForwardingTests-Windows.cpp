#include "Common.h"

#include <eacp/Core/Utils/WinInclude.h>

#include <optional>

using namespace nano;
using namespace eacp;
using namespace eacp::Graphics;

namespace
{
struct KeyLog
{
    struct Entry
    {
        bool isDown = false;
        WPARAM virtualKey = 0;
    };

    void add(UINT message, WPARAM virtualKey)
    {
        entries.add({message == WM_KEYDOWN || message == WM_SYSKEYDOWN, virtualKey});
    }

    int count(bool isDown, WPARAM virtualKey) const
    {
        return entries.countIf(
            [&](const Entry& entry)
            { return entry.isDown == isDown && entry.virtualKey == virtualKey; });
    }

    bool receivedOnePress(WPARAM virtualKey) const
    {
        return count(true, virtualKey) == 1 && count(false, virtualKey) == 1;
    }

    Vector<Entry> entries;
};

struct FakeWindowState
{
    KeyLog* log = nullptr;
    bool passesUp = false;
};

bool isKeyMessage(UINT message)
{
    return message == WM_KEYDOWN || message == WM_KEYUP || message == WM_SYSKEYDOWN
           || message == WM_SYSKEYUP;
}

LRESULT CALLBACK fakeWindowProc(HWND hwnd,
                                UINT message,
                                WPARAM wParam,
                                LPARAM lParam)
{
    auto* state =
        reinterpret_cast<FakeWindowState*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));

    if (state != nullptr && isKeyMessage(message))
    {
        state->log->add(message, wParam);

        if (state->passesUp)
            PostMessageW(GetParent(hwnd), message, wParam, lParam);

        return 0;
    }

    return DefWindowProcW(hwnd, message, wParam, lParam);
}

const wchar_t* fakeWindowClass()
{
    static auto registered = []
    {
        auto wc = WNDCLASSEXW {};
        wc.cbSize = sizeof(WNDCLASSEXW);
        wc.lpfnWndProc = fakeWindowProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = L"EacpKeyForwardingTestFake";
        RegisterClassExW(&wc);
        return true;
    }();

    (void) registered;
    return L"EacpKeyForwardingTestFake";
}

HWND makeFakeWindow(HWND parent, FakeWindowState& state)
{
    auto style =
        parent != nullptr ? DWORD {WS_CHILD | WS_VISIBLE} : DWORD {WS_POPUP};
    auto hwnd = CreateWindowExW(0,
                                fakeWindowClass(),
                                L"",
                                style,
                                0,
                                0,
                                300,
                                200,
                                parent,
                                nullptr,
                                GetModuleHandleW(nullptr),
                                nullptr);

    SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(&state));
    return hwnd;
}

struct OwnedWindow
{
    explicit OwnedWindow(HWND hwndToUse)
        : hwnd(hwndToUse)
    {
    }

    ~OwnedWindow() { DestroyWindow(hwnd); }

    OwnedWindow(const OwnedWindow&) = delete;
    OwnedWindow& operator=(const OwnedWindow&) = delete;

    HWND hwnd;
};

struct RecordedEdit
{
    KeyLog* log = nullptr;
    WNDPROC original = nullptr;
};

LRESULT CALLBACK recordingEditProc(HWND hwnd,
                                   UINT message,
                                   WPARAM wParam,
                                   LPARAM lParam)
{
    auto* edit =
        reinterpret_cast<RecordedEdit*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));

    if (isKeyMessage(message))
        edit->log->add(message, wParam);

    return CallWindowProcW(edit->original, hwnd, message, wParam, lParam);
}

bool isSpace(const KeyEvent& key)
{
    return key.keyCode == KeyCode::Space;
}

WindowOptions pluginWindowOptions()
{
    auto options = WindowOptions {};
    options.width = 300;
    options.height = 200;
    options.showInactive = true;
    return options;
}

void settle()
{
    Threads::runEventLoopFor(Time::MS {100});
}

constexpr auto firstDown = LPARAM {0x00000001};
constexpr auto repeatDown = LPARAM {0x40000001};
constexpr auto releaseUp = LPARAM {0xC0000001};
constexpr auto withAlt = LPARAM {0x20000000};

void post(HWND target, UINT message, WPARAM virtualKey, LPARAM lParam)
{
    PostMessageW(target, message, virtualKey, lParam);
    settle();
}

void keyDown(HWND target, WPARAM virtualKey, LPARAM lParam = firstDown)
{
    post(target, WM_KEYDOWN, virtualKey, lParam);
}

void keyUp(HWND target, WPARAM virtualKey)
{
    post(target, WM_KEYUP, virtualKey, releaseUp);
}

void press(HWND target, WPARAM virtualKey)
{
    keyDown(target, virtualKey);
    keyUp(target, virtualKey);
}

enum class Plugin
{
    PassesUp,
    SwallowsAll
};

struct Topology
{
    explicit Topology(Plugin kind)
    {
        embedded.setContentView(gesturesEditor);

        pluginContent.addSubview(surface);
        surface.setBounds({0.f, 0.f, 300.f, 200.f});

        pluginState.passesUp = kind == Plugin::PassesUp;
        pluginEditor.emplace(makeFakeWindow(container(), pluginState));
        SetFocus(editor());
    }

    HWND editor() { return pluginEditor->hwnd; }

    HWND container() { return (HWND) surface.getNativeParentHandle(); }

    void forwardUnhandledKeys()
    {
        surface.onUnhandledKey = [this](const NativeKeyEvent& key)
        {
            forwarder.forward(key);
            return true;
        };
    }

    void grabSpace()
    {
        grab.emplace(pluginWindow,
                     isSpace,
                     [this](const NativeKeyEvent& key) { forwarder.forward(key); });
    }

    void wireLikeGestures()
    {
        forwardUnhandledKeys();
        grabSpace();
    }

    KeyLog dawLog;
    FakeWindowState dawState {&dawLog, false};
    OwnedWindow dawPanel {makeFakeWindow(nullptr, dawState)};
    View gesturesEditor;
    EmbeddedView embedded {(void*) dawPanel.hwnd};
    EmbedderKeyForwarder forwarder {gesturesEditor};

    KeyLog pluginLog;
    FakeWindowState pluginState {&pluginLog, false};
    View pluginContent;
    NativeChildSurface surface;
    Window pluginWindow {pluginContent, pluginWindowOptions()};
    std::optional<OwnedWindow> pluginEditor;
    std::optional<KeyGrab> grab;
};
} // namespace

auto tUnusedKeyReachesTheDaw =
    test("HostedKeyForwarding/aKeyThePluginPassesUpReachesTheDaw") = []
{
    auto topology = Topology {Plugin::PassesUp};
    topology.wireLikeGestures();

    press(topology.editor(), 'A');

    check(topology.pluginLog.receivedOnePress('A'));
    check(topology.dawLog.receivedOnePress('A'));
};

auto tGrabbedSpaceSkipsThePlugin =
    test("HostedKeyForwarding/grabbedSpaceGoesToTheDawAndNotThePlugin") = []
{
    auto topology = Topology {Plugin::SwallowsAll};
    topology.wireLikeGestures();

    press(topology.editor(), VK_SPACE);

    check(topology.dawLog.receivedOnePress(VK_SPACE));
    check(topology.pluginLog.entries.empty());
};

auto tUngrabbedKeyStaysWithThePlugin =
    test("HostedKeyForwarding/ungrabbedKeyStaysWithASwallowingPlugin") = []
{
    auto topology = Topology {Plugin::SwallowsAll};
    topology.wireLikeGestures();

    press(topology.editor(), 'A');

    check(topology.pluginLog.receivedOnePress('A'));
    check(topology.dawLog.entries.empty());
};

auto tTextInputKeepsSpace =
    test("HostedKeyForwarding/focusedTextInputKeepsTheGrabbedKey") = []
{
    auto topology = Topology {Plugin::SwallowsAll};
    topology.wireLikeGestures();

    auto textLog = KeyLog {};
    auto edit = CreateWindowExW(0,
                                L"EDIT",
                                L"",
                                WS_CHILD | WS_VISIBLE,
                                0,
                                0,
                                100,
                                20,
                                topology.container(),
                                nullptr,
                                GetModuleHandleW(nullptr),
                                nullptr);
    auto recorded = RecordedEdit {&textLog, nullptr};
    SetWindowLongPtrW(edit, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(&recorded));
    recorded.original = reinterpret_cast<WNDPROC>(SetWindowLongPtrW(
        edit, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(recordingEditProc)));
    SetFocus(edit);

    press(edit, VK_SPACE);

    check(textLog.count(true, VK_SPACE) == 1);
    check(topology.dawLog.entries.empty());
    check(topology.pluginLog.entries.empty());

    DestroyWindow(edit);
};

auto tNoCallbackKeepsThePath =
    test("HostedKeyForwarding/withoutACallbackKeysGoWhereTheyAlwaysDid") = []
{
    auto topology = Topology {Plugin::PassesUp};

    press(topology.editor(), 'A');

    check(topology.pluginLog.receivedOnePress('A'));
    check(topology.dawLog.entries.empty());
};

auto tGrabIgnoresOtherWindows =
    test("HostedKeyForwarding/grabLeavesOtherWindowsAlone") = []
{
    auto topology = Topology {Plugin::SwallowsAll};
    topology.wireLikeGestures();

    auto otherLog = KeyLog {};
    auto otherState = FakeWindowState {&otherLog, false};
    auto otherContent = View {};
    auto other = Window {otherContent, pluginWindowOptions()};
    auto otherEditor =
        OwnedWindow {makeFakeWindow((HWND) other.getHandle(), otherState)};

    press(otherEditor.hwnd, VK_SPACE);

    check(otherLog.receivedOnePress(VK_SPACE));
    check(topology.dawLog.entries.empty());
};

auto tGrabbedRepeatsAllReachTheDaw =
    test("HostedKeyForwarding/everyRepeatOfAGrabbedKeyReachesTheDaw") = []
{
    auto topology = Topology {Plugin::SwallowsAll};
    topology.wireLikeGestures();

    keyDown(topology.editor(), VK_SPACE);
    keyDown(topology.editor(), VK_SPACE, repeatDown);
    keyDown(topology.editor(), VK_SPACE, repeatDown);
    keyUp(topology.editor(), VK_SPACE);

    check(topology.dawLog.count(true, VK_SPACE) == 3);
    check(topology.dawLog.count(false, VK_SPACE) == 1);
    check(topology.pluginLog.entries.empty());
};

auto tGrabDestroyedWhileHeld =
    test("HostedKeyForwarding/destroyingTheGrabMidPressStillReleasesTheDaw") = []
{
    auto topology = Topology {Plugin::SwallowsAll};
    topology.wireLikeGestures();

    keyDown(topology.editor(), VK_SPACE);
    topology.grab.reset();
    settle();
    keyUp(topology.editor(), VK_SPACE);

    check(topology.dawLog.receivedOnePress(VK_SPACE));
    check(topology.pluginLog.count(true, VK_SPACE) == 0);
};

auto tUnmappedKeysPairByVirtualKey =
    test("HostedKeyForwarding/twoUnmappedKeysHeldTogetherBothRelease") = []
{
    auto topology = Topology {Plugin::PassesUp};
    topology.wireLikeGestures();

    auto first = WPARAM {0x97};
    auto second = WPARAM {0x98};

    keyDown(topology.editor(), first);
    keyDown(topology.editor(), second);
    keyUp(topology.editor(), first);
    keyUp(topology.editor(), second);

    check(topology.dawLog.receivedOnePress(first));
    check(topology.dawLog.receivedOnePress(second));
};

auto tSysKeysStayOffTheDaw = test("HostedKeyForwarding/sysKeysAreNotForwarded") = []
{
    auto topology = Topology {Plugin::PassesUp};
    topology.wireLikeGestures();

    post(topology.editor(), WM_SYSKEYDOWN, 'A', firstDown | withAlt);
    post(topology.editor(), WM_SYSKEYUP, 'A', releaseUp | withAlt);

    check(topology.pluginLog.receivedOnePress('A'));
    check(topology.dawLog.entries.empty());
};

auto tGrabEndsWithItsObject =
    test("HostedKeyForwarding/destroyingTheGrabReleasesTheKey") = []
{
    auto topology = Topology {Plugin::SwallowsAll};
    topology.wireLikeGestures();
    topology.grab.reset();

    press(topology.editor(), VK_SPACE);

    check(topology.pluginLog.receivedOnePress(VK_SPACE));
    check(topology.dawLog.entries.empty());
};

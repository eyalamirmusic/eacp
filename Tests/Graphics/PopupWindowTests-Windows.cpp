#include "Common.h"

#include <eacp/Core/Utils/WinInclude.h>

#include <memory>

// The popup kind of Window on Win32: the menu a host opens over a plugin's
// editor.
//
// Nothing portable can check any of it. What makes a popup work is the style
// pair it is created with, the owner it is created under — set at creation or
// the shell has already given it a taskbar button — and the grab it holds,
// which is what turns a press on someone else's window into a message of ours
// to swallow. Get any of it wrong and the menu still appears, behind the
// editor it belongs to, with the host's title bar greyed out under it.

using namespace nano;
using namespace eacp::Graphics;

namespace
{
HWND nativeWindow(Window& window)
{
    return (HWND) window.getHandle();
}

DWORD styleOf(HWND hwnd)
{
    return static_cast<DWORD>(GetWindowLongPtrW(hwnd, GWL_STYLE));
}

DWORD exStyleOf(HWND hwnd)
{
    return static_cast<DWORD>(GetWindowLongPtrW(hwnd, GWL_EXSTYLE));
}

WindowOptions ownerOptions()
{
    auto options = WindowOptions {};
    options.isPrimary = false;
    options.width = 400;
    options.height = 300;
    options.initialPosition = Point {120.f, 120.f};

    return options;
}

WindowOptions popupOptions(Window& owner)
{
    auto options = WindowOptions {};
    options.popup = true;
    options.parent = &owner;
    options.width = 160;
    options.height = 120;
    options.initialPosition = Point {200.f, 200.f};

    return options;
}

// Posted rather than sent: the press that dismisses a popup is one the WndProc
// sees on its way out of the queue, and the dismissal it asks for is delivered
// a loop turn later — both of which a SendMessage would step over.
void postPress(HWND hwnd, int x, int y)
{
    PostMessageW(hwnd,
                 WM_LBUTTONDOWN,
                 MK_LBUTTON,
                 MAKELPARAM(static_cast<WORD>(x), static_cast<WORD>(y)));
}

bool waitForDismissal(const int& dismissals)
{
    auto dismissed = [&dismissals] { return dismissals > 0; };
    return eacp::Threads::runEventLoopUntil(dismissed, eacp::Time::MS {1000});
}

struct PressCounter final : View
{
    PressCounter() { setHandlesMouseEvents(true); }

    void mouseDown(const MouseEvent&) override { ++presses; }

    int presses = 0;
};
} // namespace

// WS_POPUP for the shape, the tool-window and no-activate pair for what it
// costs the window underneath, and the owner for where it sits in the stack.
auto tPopupIsAnUnactivatedToolWindow = test("Popup/isAnUnactivatedToolWindow") = []
{
    auto owner = Window {ownerOptions()};
    auto popup = Window {popupOptions(owner)};

    auto* handle = nativeWindow(popup);

    check((styleOf(handle) & WS_POPUP) != 0);
    check((styleOf(handle) & WS_CAPTION) == 0);
    check((styleOf(handle) & WS_THICKFRAME) == 0);
    check((exStyleOf(handle) & WS_EX_TOOLWINDOW) != 0);
    check((exStyleOf(handle) & WS_EX_NOACTIVATE) != 0);
    check(GetWindow(handle, GW_OWNER) == nativeWindow(owner));

    // The other half of never activating, and the half a style bit does not
    // cover: a press inside must not be the click that deactivates the owner.
    check(SendMessageW(handle, WM_MOUSEACTIVATE, 0, 0) == MA_NOACTIVATE);
};

// A host hands over the window its editor lives in, which is a child window —
// what every plugin API names as its platform type. Only a toplevel can own.
auto tNativeParentResolvesToItsToplevel =
    test("Popup/nativeParentChildResolvesToItsToplevel") = []
{
    auto owner = Window {ownerOptions()};

    auto* hostChild = CreateWindowExW(0,
                                      L"STATIC",
                                      L"",
                                      WS_CHILD,
                                      0,
                                      0,
                                      80,
                                      40,
                                      nativeWindow(owner),
                                      nullptr,
                                      nullptr,
                                      nullptr);
    check(hostChild != nullptr);

    auto options = WindowOptions {};
    options.popup = true;
    options.nativeParent = hostChild;
    options.width = 120;
    options.height = 80;

    {
        auto popup = Window {options};
        check(GetWindow(nativeWindow(popup), GW_OWNER) == nativeWindow(owner));
    }

    DestroyWindow(hostChild);
};

// The whole point of the no-activate pair. A menu that takes activation greys
// out the window it belongs to, which is how it reads as a window of its own
// rather than as part of the one underneath.
auto tOwnerKeepsActivation = test("Popup/ownerKeepsActivation") = []
{
    auto owner = Window {ownerOptions()};
    owner.toFront();

    // A headless binary, or one whose process is not the foreground one, has
    // no activation to keep: the style bits above are the guarantee that
    // stands on every run.
    if (GetActiveWindow() != nativeWindow(owner))
        return;

    auto popup = Window {popupOptions(owner)};

    check(GetActiveWindow() == nativeWindow(owner));
    check(GetForegroundWindow() != nativeWindow(popup));
};

// A press outside asks for the dismissal and is eaten on the way: clicking a
// menu away never also presses what was under it, which is the behaviour every
// menu on the platform has. With the grab held, that press arrives here in the
// popup's own client coordinates, negative ones included.
auto tOutsidePressAsksForDismissal = test("Popup/outsidePressAsksForDismissal") = []
{
    auto inOwner = PressCounter {};
    auto owner = Window {inOwner, ownerOptions()};

    auto inPopup = PressCounter {};
    auto popup = Window {inPopup, popupOptions(owner)};

    auto dismissals = 0;
    popup.events.onDismissRequested = [&dismissals] { ++dismissals; };

    postPress(nativeWindow(popup), -40, -40);

    check(waitForDismissal(dismissals));
    check(dismissals == 1);
    check(inPopup.presses == 0);
    check(inOwner.presses == 0);
};

// A press inside is the menu's own and must reach it, or no item could ever be
// chosen.
auto tPressInsideIsNotADismissal = test("Popup/pressInsideIsNotADismissal") = []
{
    auto owner = Window {ownerOptions()};
    auto popup = Window {popupOptions(owner)};

    auto dismissals = 0;
    popup.events.onDismissRequested = [&dismissals] { ++dismissals; };

    postPress(nativeWindow(popup), 8, 8);
    eacp::Threads::runEventLoopFor(eacp::Time::MS {150});

    check(dismissals == 0);
};

// The tooltip case: the app takes its popup down itself and the press belongs
// to whatever it landed on.
auto tOutsidePressIsKeptWhenAsked = test("Popup/outsidePressIsKeptWhenAsked") = []
{
    auto owner = Window {ownerOptions()};

    auto options = popupOptions(owner);
    options.dismissOnOutsideClick = false;

    auto popup = Window {options};

    auto dismissals = 0;
    popup.events.onDismissRequested = [&dismissals] { ++dismissals; };

    postPress(nativeWindow(popup), -40, -40);
    eacp::Threads::runEventLoopFor(eacp::Time::MS {150});

    check(dismissals == 0);
};

// Escape reaches the window that has the focus, and a popup never takes it: it
// arrives in the owner and is spent on the menu over it rather than reaching
// the owner's own views.
auto tEscapeInTheOwnerDismissesIt = test("Popup/escapeInTheOwnerDismissesIt") = []
{
    auto owner = Window {ownerOptions()};
    auto popup = Window {popupOptions(owner)};

    auto dismissals = 0;
    popup.events.onDismissRequested = [&dismissals] { ++dismissals; };

    PostMessageW(nativeWindow(owner), WM_KEYDOWN, VK_ESCAPE, 0);

    check(waitForDismissal(dismissals));
    check(dismissals == 1);
};

// The owner losing activation takes its menus with it — the user has gone to
// another window, and a menu left floating over an inactive one is the bug
// every host reports.
auto tOwnerGoingInactiveDismissesIt =
    test("Popup/ownerGoingInactiveDismissesIt") = []
{
    auto owner = Window {ownerOptions()};
    auto popup = Window {popupOptions(owner)};

    auto dismissals = 0;
    popup.events.onDismissRequested = [&dismissals] { ++dismissals; };

    PostMessageW(nativeWindow(owner), WM_ACTIVATE, MAKEWPARAM(WA_INACTIVE, 0), 0);

    check(waitForDismissal(dismissals));
    check(dismissals == 1);
};

// The order an app hits on quit: the owner is torn down with the menu still
// up. Win32 destroys an owned window with its owner, so what must not happen
// is the popup being left holding a pointer to a Native that has gone.
auto tOwnerMayGoFirst = test("Popup/survivesTheOwnerGoingFirst") = []
{
    auto popup = std::unique_ptr<Window> {};
    auto dismissals = 0;

    {
        auto owner = Window {ownerOptions()};
        popup = std::make_unique<Window>(popupOptions(owner));
        popup->events.onDismissRequested = [&dismissals] { ++dismissals; };
    }

    check(waitForDismissal(dismissals));
    check(dismissals == 1);

    popup.reset();
};

// And the other order: the menu is built and thrown away on every open, so the
// owner must not be left holding the popup either — an Escape afterwards would
// otherwise reach a Native that has gone.
auto tPopupMayGoFirst = test("Popup/ownerSurvivesThePopupGoingFirst") = []
{
    auto owner = Window {ownerOptions()};

    {
        auto popup = Window {popupOptions(owner)};
    }

    PostMessageW(nativeWindow(owner), WM_KEYDOWN, VK_ESCAPE, 0);
    eacp::Threads::runEventLoopFor(eacp::Time::MS {150});
};

// Destroyed from inside the handler, which is the lifetime a menu wants and
// the one that would take the callback down with it if the fire were not
// deferred and the function not copied first.
auto tDismissalMayDestroyTheWindow = test("Popup/dismissalMayDestroyTheWindow") = []
{
    auto owner = Window {ownerOptions()};
    auto popup = std::make_unique<Window>(popupOptions(owner));

    auto dismissals = 0;
    auto* popupPointer = &popup;

    popup->events.onDismissRequested = [popupPointer, &dismissals]
    {
        ++dismissals;
        popupPointer->reset();
    };

    postPress(nativeWindow(*popup), -40, -40);

    check(waitForDismissal(dismissals));
    check(popup == nullptr);
};

// A parent without the popup flag is an owned window and nothing more: held
// above its owner and off the taskbar, but titled, activating and its own
// window in every other way.
auto tOwnedWindowNeedNotBeAPopup = test("Popup/ownedWindowNeedNotBeAPopup") = []
{
    auto owner = Window {ownerOptions()};

    auto options = ownerOptions();
    options.parent = &owner;

    auto owned = Window {options};
    auto* handle = nativeWindow(owned);

    check(GetWindow(handle, GW_OWNER) == nativeWindow(owner));
    check((styleOf(handle) & WS_CAPTION) != 0);
    check((exStyleOf(handle) & WS_EX_NOACTIVATE) == 0);
};

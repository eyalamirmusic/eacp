#include "Common.h"

using namespace nano;
using namespace eacp::Graphics;

// Mouse lock is intent-based: setMouseLocked records the desired state and
// the OS-level lock only engages while the window has key focus, so the
// intent must be togglable (and readable) on a window that never becomes
// key, like here.
auto tMouseLockIntent = test("Window/mouseLockIntentToggles") = []
{
    auto window = Window {};

    check(!window.isMouseLocked());

    window.setMouseLocked(true);
    check(window.isMouseLocked());

    window.setMouseLocked(true);
    check(window.isMouseLocked());

    window.setMouseLocked(false);
    check(!window.isMouseLocked());
};

auto tSecondaryWindowDefaultQuitIsNoOp =
    test("WindowOptions/secondaryDefaultQuitIsNoOp") = []
{
    auto options = WindowOptions {};
    options.isPrimary = false;

    auto callback = options.effectiveOnQuit();
    check(static_cast<bool>(callback));
    callback();
};

auto tActivationChangedCallbackIsUserOwned =
    test("WindowEvents/activationChangedCallbackIsUserOwned") = []
{
    auto events = WindowEvents {};
    auto lastState = false;
    auto calls = 0;

    events.onActivationChanged = [&](bool isKey)
    {
        lastState = isKey;
        ++calls;
    };

    events.onActivationChanged(true);
    events.onActivationChanged(false);

    check(calls == 2);
    check(!lastState);
};

auto tWindowOptionsNewAffordancesDefaultOff =
    test("WindowOptions/newAffordancesDefaultOff") = []
{
    auto options = WindowOptions {};

    check(!options.ignoresMouseEvents);
    check(!options.showInactive);
    check(!options.visibleOnAllWorkspaces);
};

// The icons are bring-your-own, and the providers are never null: the
// defaults are callable and return an invalid Image, which keeps the
// system default without any null checks at the call sites.
auto tIconProvidersDefaultToInvalidImage =
    test("WindowOptions/iconProvidersDefaultToInvalidImage") = []
{
    check(!WindowOptions {}.applicationIcon());
    check(!WindowOptions {}.altTabIcon());
};

// Live behaviour (the icon actually landing on the window / Dock) is
// demonstrated by Apps/WebView/Browser; a provided icon just has to be
// safe on a window that never materializes.
auto tApplicationIconConstructsUnderHeadless =
    test("WindowOptions/applicationIconConstructsUnderHeadless") = []
{
    auto options = WindowOptions {};
    options.isPrimary = false;
    options.applicationIcon = [] { return Image {8, 8}; };

    auto window = Window {options};
    check(true);
};

auto tGlobalHotKeyConstructsUnderHeadless =
    test("GlobalHotKey/headlessConstructionIsInert") = []
{
    auto& environment = eacp::Apps::getAppEnvironment();
    auto previousHeadless = environment.headless;
    environment.headless = true;

    auto calls = 0;
    {
        auto hotKey = GlobalHotKey {ModifierKeys {.alt = true, .command = true},
                                    KeyCode::L,
                                    [&] { ++calls; }};
    }

    check(calls == 0);
    environment.headless = previousHeadless;
};

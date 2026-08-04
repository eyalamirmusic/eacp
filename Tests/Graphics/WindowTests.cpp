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

// The platform delegates call onHidden straight off the event struct, so an
// app that never assigns one must still be safe to close.
auto tHiddenCallbackDefaultsToCallableNoOp =
    test("WindowEvents/hiddenDefaultsToCallableNoOp") = []
{
    auto events = WindowEvents {};

    check(static_cast<bool>(events.onHidden));
    events.onHidden();

    auto calls = 0;
    events.onHidden = [&] { ++calls; };
    events.onHidden();

    check(calls == 1);
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
// Fullscreen is the one resize a locked ratio cannot survive, so setting a
// ratio is also a statement about fullscreen - unless the app says otherwise.
auto tFullScreenFollowsAspectRatio =
    test("WindowOptions/fullScreenClosesWithTheRatioLock") = []
{
    auto options = WindowOptions {};
    check(options.effectiveAllowsFullScreen());

    options.aspectRatio = Point {16.f, 9.f};
    check(!options.effectiveAllowsFullScreen());

    options.allowsFullScreen = true;
    check(options.effectiveAllowsFullScreen());

    options.allowsFullScreen = false;
    options.aspectRatio.reset();
    check(!options.effectiveAllowsFullScreen());
};

// Both platforms skip a ratio that describes no shape, so the default has to
// agree with them: a zero or negative side must not cost the window its
// fullscreen on the strength of a constraint nobody is enforcing.
auto tDegenerateAspectRatioIsNoRatio =
    test("WindowOptions/degenerateAspectRatioIsNotALock") = []
{
    auto options = WindowOptions {};

    for (auto ratio: {Point {0.f, 0.f}, Point {16.f, 0.f}, Point {-16.f, 9.f}})
    {
        options.aspectRatio = ratio;
        check(!options.hasAspectRatio());
        check(options.effectiveAllowsFullScreen());
    }

    options.aspectRatio = Point {1920.f, 1080.f};
    check(options.hasAspectRatio());
};

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

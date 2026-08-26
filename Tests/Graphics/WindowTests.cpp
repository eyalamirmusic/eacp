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

// Window position, in the screen points WindowOptions::initialPosition and
// Display are already in: top-left origin, y growing down. The window is made
// under headless too — it is only never ordered front — so its frame is real
// and these read it rather than a stub.

auto tMovedCallbackIsUserOwned = test("WindowEvents/movedCallbackIsUserOwned") = []
{
    auto events = WindowEvents {};
    auto lastPosition = Point {};
    auto calls = 0;

    events.onMoved = [&](Point position)
    {
        lastPosition = position;
        ++calls;
    };

    events.onMoved({40.f, 90.f});

    check(calls == 1);
    check(lastPosition.x == 40.f);
    check(lastPosition.y == 90.f);
};

// Non-null by default, so a backend reporting a move never has to check.
auto tMovedDefaultsToNoOp = test("WindowEvents/movedDefaultsToNoOp") = []
{
    auto events = WindowEvents {};

    check(static_cast<bool>(events.onMoved));
    events.onMoved({0.f, 0.f});
};

auto tInitialPositionIsReadBack = test("Window/initialPositionIsReadBack") = []
{
    auto options = WindowOptions {};
    options.initialPosition = Point {120.f, 90.f};

    auto window = Window {options};
    auto position = window.getPosition();

    check(position.x == 120.f);
    check(position.y == 90.f);
};

auto tSetPositionMovesTheWindow = test("Window/setPositionMovesTheWindow") = []
{
    auto window = Window {};

    window.setPosition({210.f, 160.f});

    auto position = window.getPosition();

    check(position.x == 210.f);
    check(position.y == 160.f);
};

// The round trip is the whole point: what getPosition reports has to be a
// value setPosition and initialPosition accept, or a window cannot be put
// back where the user left it.
auto tPositionRoundTrips = test("Window/positionRoundTrips") = []
{
    auto first = Window {};
    first.setPosition({260.f, 200.f});

    auto options = WindowOptions {};
    options.initialPosition = first.getPosition();

    auto second = Window {options};

    check(second.getPosition().x == first.getPosition().x);
    check(second.getPosition().y == first.getPosition().y);
};

// A programmatic move is still a move: an app that saves its window position
// from onMoved would otherwise miss every placement it made itself.
auto tSetPositionFiresOnMoved = test("Window/setPositionFiresOnMoved") = []
{
    auto window = Window {};
    auto calls = 0;
    auto reported = Point {};

    window.events.onMoved = [&](Point position)
    {
        ++calls;
        reported = position;
    };

    window.setPosition({300.f, 220.f});

    check(calls == 1);
    check(reported.x == 300.f);
    check(reported.y == 220.f);
};

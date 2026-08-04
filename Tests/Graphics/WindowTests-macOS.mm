#import <AppKit/AppKit.h>

#include "Common.h"

// The platform half of the fullscreen opt-out. WindowTests.cpp covers the
// policy - which windows should lose fullscreen - and this covers the only
// thing that makes the policy true: the bit reaching the NSWindow.
//
// Worth its own test because the failure is silent in both directions. A
// collectionBehavior that never gets set leaves an aspect-locked window one
// green-button click away from the shape it exists to refuse, and a mask
// written over the top of the Spaces behaviour takes an always-visible window
// off the other Spaces. Nothing else fails when either happens.

using namespace nano;
// Carbon's QuickDraw Point is visible through AppKit, so eacp's has to be
// spelled out here.
using namespace eacp::Graphics;

NSWindow* nativeWindow(Window& window)
{
    return (__bridge NSWindow*) window.getHandle();
}

auto tRatioLockDeniesFullScreen = test("Window/ratioLockSetsFullScreenNone") = []
{
    auto options = WindowOptions {};
    options.isPrimary = false;
    options.aspectRatio = eacp::Graphics::Point {16.f, 9.f};

    auto window = Window {options};
    auto behavior = nativeWindow(window).collectionBehavior;

    check((behavior & NSWindowCollectionBehaviorFullScreenNone) != 0);

    // FullScreenNone shares its slot with the two opt-ins; leaving one behind
    // is an undefined combination rather than a stricter one.
    check((behavior & NSWindowCollectionBehaviorFullScreenPrimary) == 0);
    check((behavior & NSWindowCollectionBehaviorFullScreenAuxiliary) == 0);

    // The ratio itself still has to be on the window - AppKit is what enforces
    // it, on every resize path the opt-out leaves standing.
    check(NSEqualSizes(nativeWindow(window).contentAspectRatio,
                       NSMakeSize(16.f, 9.f)));
};

auto tPlainWindowKeepsFullScreen = test("Window/plainWindowKeepsFullScreen") = []
{
    auto options = WindowOptions {};
    options.isPrimary = false;

    auto window = Window {options};

    check((nativeWindow(window).collectionBehavior
           & NSWindowCollectionBehaviorFullScreenNone)
          == 0);
};

auto tExplicitOptInWinsOverTheRatio =
    test("Window/allowsFullScreenOverridesTheRatio") = []
{
    auto options = WindowOptions {};
    options.isPrimary = false;
    options.aspectRatio = eacp::Graphics::Point {16.f, 9.f};
    options.allowsFullScreen = true;

    auto window = Window {options};

    check((nativeWindow(window).collectionBehavior
           & NSWindowCollectionBehaviorFullScreenNone)
          == 0);
};

// An all-Spaces window is auxiliary to somebody else's fullscreen, which is
// already not a window that takes the screen itself. The opt-out must not
// trade the Spaces behaviour away to say so a second time.
auto tAllWorkspacesKeepsItsSpacesBehaviour =
    test("Window/allWorkspacesSurvivesTheRatioLock") = []
{
    auto options = WindowOptions {};
    options.isPrimary = false;
    options.aspectRatio = eacp::Graphics::Point {16.f, 9.f};
    options.visibleOnAllWorkspaces = true;

    auto window = Window {options};
    auto behavior = nativeWindow(window).collectionBehavior;

    check((behavior & NSWindowCollectionBehaviorCanJoinAllSpaces) != 0);
    check((behavior & NSWindowCollectionBehaviorFullScreenAuxiliary) != 0);
    check((behavior & NSWindowCollectionBehaviorFullScreenNone) == 0);
};

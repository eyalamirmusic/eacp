#import <AppKit/AppKit.h>

#include "Common.h"

// The platform half of the size constraint and the fullscreen opt-out.
// SizeConstraintTests covers the arithmetic and WindowTests.cpp the policy;
// this covers the only things that make either true on macOS: the delegate
// selectors AppKit consults before a drag, a zoom and a fullscreen, and the
// collectionBehavior bit the opt-out sets.
//
// Worth its own test because the failure is silent in both directions. A
// selector that never gets registered leaves a constrained window one drag
// away from the shape it exists to refuse, and a mask written over the top of
// the Spaces behaviour takes an always-visible window off the other Spaces.
// Nothing else fails when either happens.

using namespace nano;
// Carbon's QuickDraw Point is visible through AppKit, so eacp's has to be
// spelled out here.
using namespace eacp::Graphics;

NSWindow* nativeWindow(Window& window)
{
    return (__bridge NSWindow*) window.getHandle();
}

id<NSWindowDelegate> delegateOf(Window& window)
{
    return nativeWindow(window).delegate;
}

NSSize contentSizeOf(Window& window, NSSize frameSize)
{
    auto frame = NSMakeRect(0, 0, frameSize.width, frameSize.height);
    return [nativeWindow(window) contentRectForFrameRect:frame].size;
}

WindowOptions lockedOptions()
{
    auto options = WindowOptions {};
    options.isPrimary = false;
    options.width = 960;
    options.height = 540;
    options.sizeConstraint = AspectRatioLock {eacp::Graphics::Point {16.f, 9.f}};
    return options;
}

// A user drag: AppKit asks windowWillResize:toSize: with a frame size, and
// the answer has to be the constrained one, still as a frame size.
auto tDragIsConstrained = test("Window/dragIsConstrainedThroughTheDelegate") = []
{
    auto window = Window {lockedOptions()};
    auto* native = nativeWindow(window);

    // Only the width moves, so the width drives.
    auto wider = [native frameRectForContentRect:NSMakeRect(0, 0, 1600, 540)].size;
    auto answer = [delegateOf(window) windowWillResize:native toSize:wider];
    auto content = contentSizeOf(window, answer);

    check(content.width == 1600);
    check(content.height == 900);
};

// The green button's zoom never passes through windowWillResize, so it has a
// selector of its own: the largest allowed size inside the default frame,
// kept to that frame's top-left.
auto tZoomIsFitted = test("Window/zoomIsFittedThroughTheDelegate") = []
{
    auto window = Window {lockedOptions()};
    auto* native = nativeWindow(window);

    auto tooWide = [native frameRectForContentRect:NSMakeRect(100, 100, 3000, 540)];
    auto answer = [delegateOf(window) windowWillUseStandardFrame:native
                                                     defaultFrame:tooWide];
    auto content = contentSizeOf(window, answer.size);

    check(content.width == 960);
    check(content.height == 540);
    check(answer.origin.x == tooWide.origin.x);
    check(NSMaxY(answer) == NSMaxY(tooWide));
};

// Fullscreen hands the window the display; the delegate hands back the largest
// allowed size and AppKit centres it on black.
auto tFullScreenIsFitted = test("Window/fullScreenContentIsFittedThroughTheDelegate") =
    []
{
    auto window = Window {lockedOptions()};

    auto answer = [delegateOf(window) window:nativeWindow(window)
                     willUseFullScreenContentSize:NSMakeSize(2000, 540)];

    check(answer.width == 960);
    check(answer.height == 540);
};

// The size the window opens at is already an allowed one.
auto tWindowOpensAtASnappedSize =
    test("Window/initialSizeIsSnappedToTheConstraint") = []
{
    auto options = lockedOptions();
    options.height = 100;

    auto window = Window {options};
    auto content = contentSizeOf(window, nativeWindow(window).frame.size);

    check(content.width == 960);
    check(content.height == 540);
};

// The flicker: a corner dragged straight down proposes the start width every
// time. Classified against the size the window holds - which the constraint
// has just moved - the axis flips on each event and the window alternates
// between two shapes. Classified against the drag's start size, the same
// proposal gets the same answer whatever the window holds at the time.
auto tSameProposalSameAnswer =
    test("Window/sameProposalGetsTheSameAnswerThroughoutADrag") = []
{
    auto window = Window {lockedOptions()};
    auto* native = nativeWindow(window);
    auto delegate = delegateOf(window);

    [delegate windowWillStartLiveResize:
                  [NSNotification notificationWithName:NSWindowWillStartLiveResizeNotification
                                                object:native]];

    // Straight down from the corner: the width is the start width, so the
    // height drives and the width follows.
    auto proposal = [native frameRectForContentRect:NSMakeRect(0, 0, 960, 720)].size;
    auto first = contentSizeOf(window, [delegate windowWillResize:native
                                                           toSize:proposal]);
    check(first.width == 1280);
    check(first.height == 720);

    // The window now holds the answer, and the next event proposes the same
    // size again.
    [native setContentSize:first];
    auto second = contentSizeOf(window, [delegate windowWillResize:native
                                                            toSize:proposal]);
    check(second.width == first.width);
    check(second.height == first.height);

    [delegate windowDidEndLiveResize:
                  [NSNotification notificationWithName:NSWindowDidEndLiveResizeNotification
                                                object:native]];
};

// A whole-content ratio lock takes fullscreen with it, and the bit has to
// reach the NSWindow.
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

// A sizeConstraint alone has its own letterbox path, so it keeps fullscreen.
auto tConstrainedWindowKeepsFullScreen =
    test("Window/sizeConstraintAloneKeepsFullScreen") = []
{
    auto window = Window {lockedOptions()};

    check((nativeWindow(window).collectionBehavior
           & NSWindowCollectionBehaviorFullScreenNone)
          == 0);
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

#import <AppKit/AppKit.h>

#include "Common.h"

#include <eacp/Core/ObjC/ObjC.h>

#include <memory>

// The popup kind of Window: the menu a host opens over a plugin's editor.
//
// Nothing portable can check any of it. What makes a popup work is which
// AppKit class it is — the nonactivating and HUD masks do nothing at all on a
// plain NSWindow, which is what made those flags dead — whose child window it
// is, and that neither of those costs the owner its key status. Get any of it
// wrong and the menu still appears, in front of nothing, with the window
// behind it greyed out.

using namespace nano;
using namespace eacp::Graphics;

namespace
{
NSWindow* nativeWindow(Window& window)
{
    return (__bridge NSWindow*) window.getHandle();
}

WindowOptions ownerOptions()
{
    auto options = WindowOptions {};
    options.isPrimary = false;
    options.width = 400;
    options.height = 300;
    options.initialPosition = eacp::Graphics::Point {120.f, 120.f};

    return options;
}

WindowOptions popupOptions(Window& owner)
{
    auto options = WindowOptions {};
    options.popup = true;
    options.parent = &owner;
    options.width = 160;
    options.height = 120;
    options.initialPosition = eacp::Graphics::Point {200.f, 200.f};

    return options;
}

// The location an NSEvent carries is in the coordinates of the window it
// names — not the screen, unless it names no window. A press built the other
// way round lands nowhere and is delivered to nothing, which is a test that
// passes by accident.
NSEvent* pressAt(NSPoint windowLocation, NSInteger windowNumber)
{
    return [NSEvent mouseEventWithType:NSEventTypeLeftMouseDown
                              location:windowLocation
                         modifierFlags:0
                             timestamp:0
                          windowNumber:windowNumber
                               context:nil
                           eventNumber:0
                            clickCount:1
                              pressure:1.f];
}

NSEvent* escapePress()
{
    return [NSEvent keyEventWithType:NSEventTypeKeyDown
                            location:NSZeroPoint
                       modifierFlags:0
                           timestamp:0
                        windowNumber:0
                             context:nil
                          characters:@"\033"
         charactersIgnoringModifiers:@"\033"
                           isARepeat:NO
                             keyCode:KeyCode::Escape];
}

// Posted rather than sent: a local monitor sees an event on its way out of the
// application's queue, which is the path a real press takes and the only one
// that proves the monitor is installed.
bool postAndWaitForDismissal(NSEvent* event, const int& dismissals)
{
    [NSApp postEvent:event atStart:YES];

    auto dismissed = [&dismissals] { return dismissals > 0; };
    return eacp::Threads::runEventLoopUntil(dismissed, eacp::Time::MS {1000});
}

constexpr short sentinelSubtype = 0x4541;

NSEvent* sentinelEvent()
{
    return [NSEvent otherEventWithType:NSEventTypeApplicationDefined
                              location:NSZeroPoint
                         modifierFlags:0
                             timestamp:0
                          windowNumber:0
                               context:nil
                               subtype:sentinelSubtype
                                 data1:0
                                 data2:0];
}

// How to wait for an absence without a sleep to guess at. A sentinel event
// posted behind the press is dispatched after it, and a callAsync queued once
// the sentinel has arrived runs behind anything the monitor queued while the
// press went past — so by the time this returns, everything that press was
// going to cause has happened.
void drainAfterPost()
{
    auto seen = false;
    auto* seenFlag = &seen;

    auto watch = ^NSEvent*(NSEvent* event)
    {
        if (event.subtype == sentinelSubtype)
            *seenFlag = true;

        return event;
    };

    id monitor = [NSEvent
        addLocalMonitorForEventsMatchingMask:NSEventMaskApplicationDefined
                                     handler:watch];

    [NSApp postEvent:sentinelEvent() atStart:NO];

    auto sentinelArrived = [seenFlag] { return *seenFlag; };
    eacp::Threads::runEventLoopUntil(sentinelArrived, eacp::Time::MS {1000});

    [NSEvent removeMonitor:monitor];

    auto delivered = false;
    auto mark = [&delivered] { delivered = true; };
    eacp::Threads::callAsync(mark);

    auto callbacksRan = [&delivered] { return delivered; };
    eacp::Threads::runEventLoopUntil(callbacksRan, eacp::Time::MS {1000});
}

struct PressCounter final : View
{
    PressCounter() { setHandlesMouseEvents(true); }

    void mouseDown(const MouseEvent&) override { ++presses; }

    int presses = 0;
};

// AppKit routes a press to a window that is really on screen and to no other,
// so a case that asks whether a press was delivered has to put one there —
// the suite is headless, and Window::setVisible is headless-gated, so the
// order-in is made directly. Ordered, not activated: the owner is no more key
// than before, which is the state a popup lives in anyway. Child windows come
// in with their owner, so popups made after this are on screen too.
void orderInForRealPresses(Window& window)
{
    [nativeWindow(window) orderFront:nil];

    // And a turn of the loop before anything is posted: the window server is
    // only told about the window here, and a press posted in the same breath
    // as the order-in is delivered to nothing.
    drainAfterPost();
}

// A popup of its own, away from the frame of the one the case is about, so
// what a press in it means is decided by which window it landed in rather
// than by where the two happen to overlap.
WindowOptions secondPopupOptions(Window& parent)
{
    auto options = popupOptions(parent);
    options.initialPosition = eacp::Graphics::Point {420.f, 260.f};

    return options;
}
} // namespace

// The class, not just the mask: NSWindowStyleMaskNonactivatingPanel is
// NSPanel's, and an NSWindow given it carries the bit and behaves as though it
// had never been set.
auto tPopupIsANonactivatingPanel = test("Popup/isANonactivatingPanel") = []
{
    auto owner = Window {ownerOptions()};
    auto popup = Window {popupOptions(owner)};

    auto* panel = nativeWindow(popup);

    check([panel isKindOfClass:[NSPanel class]]);
    check((panel.styleMask & NSWindowStyleMaskNonactivatingPanel) != 0);
    check((panel.styleMask & NSWindowStyleMaskTitled) == 0);
    check(!panel.canBecomeKeyWindow);
    check(!panel.canBecomeMainWindow);
    check(panel.level == NSPopUpMenuWindowLevel);
    check(!panel.hidesOnDeactivate);
};

// A child window takes its owner's level when it is ordered back in, so a
// popup hidden and shown again comes back level with the window it exists to
// cover — and the foreign native content in that window covers it.
//
// Needs the window really ordered out and back, so the headless flag the suite
// runs under comes off for the length of the case.
auto tLevelSurvivesAHideAndShow = test("Popup/levelSurvivesAHideAndShow") = []
{
    auto& environment = eacp::Apps::getAppEnvironment();
    auto wasHeadless = environment.headless;

    auto owner = Window {ownerOptions()};
    auto popup = Window {popupOptions(owner)};

    environment.headless = false;
    popup.setVisible(false);
    popup.setVisible(true);
    environment.headless = wasHeadless;

    check(nativeWindow(popup).level == NSPopUpMenuWindowLevel);
};

// The same fix reaches the flags that asked for a panel the long way round.
auto tPanelFlagsSelectAPanel = test("Popup/panelFlagsSelectAnNSPanel") = []
{
    auto options = WindowOptions {};
    options.isPrimary = false;
    options.flags.add(WindowFlags::NonactivatingPanel);

    auto window = Window {options};
    check([nativeWindow(window) isKindOfClass:[NSPanel class]]);
};

// Ordered above its owner, moving and hiding with it: all of which is what
// being a child window means, and none of which an app tracking onMoved by
// hand does as smoothly.
auto tPopupIsAChildOfItsOwner = test("Popup/isAChildOfItsOwner") = []
{
    auto owner = Window {ownerOptions()};
    auto popup = Window {popupOptions(owner)};

    check(nativeWindow(popup).parentWindow == nativeWindow(owner));
    check([nativeWindow(owner).childWindows
        containsObject:nativeWindow(popup)]);
};

// A host that is not an eacp app hands over a view, not a window — which is
// what every plugin API names as its platform type.
auto tNativeParentViewResolvesToItsWindow =
    test("Popup/nativeParentViewResolvesToItsWindow") = []
{
    auto host = eacp::ObjC::Ptr<NSWindow> {[[NSWindow alloc]
        initWithContentRect:NSMakeRect(100, 100, 300, 200)
                  styleMask:NSWindowStyleMaskTitled
                    backing:NSBackingStoreBuffered
                      defer:NO]};

    [host.get() setReleasedWhenClosed:NO];

    auto* hostView = [[NSView alloc] initWithFrame:NSMakeRect(0, 0, 300, 200)];
    [host.get() setContentView:hostView];
    [hostView release];

    auto options = WindowOptions {};
    options.popup = true;
    options.nativeParent = (__bridge void*) hostView;
    options.width = 120;
    options.height = 80;

    auto popup = Window {options};
    check(nativeWindow(popup).parentWindow == host.get());
};

// The whole point of the nonactivating panel. A menu that takes key status
// greys out the window it belongs to, which is how it reads as a window of its
// own rather than as part of the one underneath.
auto tOwnerKeepsKeyStatus = test("Popup/ownerKeepsKeyStatus") = []
{
    auto owner = Window {ownerOptions()};
    auto* ownerNative = nativeWindow(owner);

    [ownerNative makeKeyAndOrderFront:nil];

    // A headless binary that is not the active application never gets key
    // status to keep, so there is nothing to assert here and the panel's
    // refusal above is the guarantee that stands on every run.
    if (!ownerNative.isKeyWindow)
        return;

    auto popup = Window {popupOptions(owner)};

    check(ownerNative.isKeyWindow);
    check(NSApp.keyWindow != nativeWindow(popup));
};

// The other half of never being key. A tracking area that is only active in
// the key window never fires inside a popup, so its items would not highlight
// under the pointer — which is most of what a menu is, and the one thing
// nothing else in the suite would catch.
auto tPopupViewsTrackTheMouseAnyway =
    test("Popup/viewsTrackTheMouseWhileNotKey") = []
{
    auto owner = Window {ownerOptions()};

    auto inPopup = View {};
    auto popup = Window {inPopup, popupOptions(owner)};

    auto inOwnerWindow = View {};
    owner.setContentView(inOwnerWindow);

    auto trackingOptions = [](View& view)
    {
        auto* native = (__bridge NSView*) view.getHandle();
        [native updateTrackingAreas];

        check(native.trackingAreas.count > 0);
        return ((NSTrackingArea*) native.trackingAreas.firstObject).options;
    };

    check((trackingOptions(inPopup) & NSTrackingActiveAlways) != 0);
    check((trackingOptions(inPopup) & NSTrackingActiveInKeyWindow) == 0);

    // And an ordinary window is left as it was: a view in one tracks while
    // its window is key and not while some other app's is.
    check((trackingOptions(inOwnerWindow) & NSTrackingActiveInKeyWindow) != 0);
    check((trackingOptions(inOwnerWindow) & NSTrackingActiveAlways) == 0);
};

// A press outside asks for the dismissal and is eaten on the way: clicking a
// menu away never also presses what was under it, which is the behaviour every
// menu on the platform has.
auto tOutsidePressAsksForDismissal = test("Popup/outsidePressAsksForDismissal") = []
{
    auto content = PressCounter {};
    auto owner = Window {content, ownerOptions()};
    orderInForRealPresses(owner);

    auto* ownerNative = nativeWindow(owner);
    auto press = pressAt(NSMakePoint(40.f, 40.f), ownerNative.windowNumber);

    auto dismissals = 0;

    {
        auto popup = Window {popupOptions(owner)};
        popup.events.onDismissRequested = [&dismissals] { ++dismissals; };

        check(postAndWaitForDismissal(press, dismissals));
        check(dismissals == 1);

        // Eaten on the way past: the press that closed the menu never reached
        // the view it landed on.
        check(content.presses == 0);
    }

    // And the same press with no menu up does reach it, which is what makes
    // the zero above a statement about the monitor rather than about the test.
    [NSApp postEvent:press atStart:YES];
    drainAfterPost();

    check(content.presses == 1);
    check(dismissals == 1);
};

// A menu opened from inside a press ends that press for the owner: the up
// belongs to the menu now, and the view it went down in would otherwise wait
// for one that is never coming.
auto tOpeningEndsTheOwnersPress = test("Popup/openingItEndsTheOwnersPress") = []
{
    struct Pressable final : View
    {
        Pressable() { setHandlesMouseEvents(true); }

        void mouseDown(const MouseEvent&) override { ++downs; }
        void mouseUp(const MouseEvent&) override { ++ups; }

        int downs = 0;
        int ups = 0;
    };

    auto content = View {};
    auto pressable = Pressable {};

    content.addSubview(pressable);

    auto owner = Window {content, ownerOptions()};
    pressable.setBounds({0.f, 0.f, 100.f, 50.f});

    auto press = MouseEvent {};
    press.type = MouseEventType::Down;
    press.pos = {10.f, 10.f};
    press.downPos = press.pos;
    content.dispatchMouseEvent(press);
    check(pressable.downs == 1);

    auto popup = Window {popupOptions(owner)};

    auto release = press;
    release.type = MouseEventType::Up;
    content.dispatchMouseEvent(release);

    check(pressable.ups == 0);
};

// Escape, on the same monitor and swallowed for the same reason.
auto tEscapeAsksForDismissal = test("Popup/escapeAsksForDismissal") = []
{
    auto owner = Window {ownerOptions()};
    auto popup = Window {popupOptions(owner)};

    auto dismissals = 0;
    popup.events.onDismissRequested = [&dismissals] { ++dismissals; };

    check(postAndWaitForDismissal(escapePress(), dismissals));
    check(dismissals == 1);
};

// A press inside is the menu's own and must reach it, or no item could ever
// be chosen.
auto tPressInsideIsLeftAlone = test("Popup/pressInsideIsNotADismissal") = []
{
    auto owner = Window {ownerOptions()};
    orderInForRealPresses(owner);

    auto inPopup = PressCounter {};
    auto popup = Window {inPopup, popupOptions(owner)};
    orderInForRealPresses(popup);

    auto dismissals = 0;
    popup.events.onDismissRequested = [&dismissals] { ++dismissals; };

    auto* popupNative = nativeWindow(popup);
    auto inside = NSMakePoint(10.f, 10.f);

    [NSApp postEvent:pressAt(inside, popupNative.windowNumber) atStart:YES];
    drainAfterPost();

    check(dismissals == 0);
    check(inPopup.presses == 1);
};

// A held Escape repeats, and a click into another app is reported twice over —
// by the global monitor and by the app deactivating. One showing asks once, or
// a handler that only hides the popup is called again for a menu that has
// already gone.
auto tSecondEscapeAsksNothingNew = test("Popup/aSecondEscapeAsksNothingNew") = []
{
    auto owner = Window {ownerOptions()};
    auto popup = Window {popupOptions(owner)};

    auto dismissals = 0;
    popup.events.onDismissRequested = [&dismissals] { ++dismissals; };

    [NSApp postEvent:escapePress() atStart:NO];
    [NSApp postEvent:escapePress() atStart:NO];
    drainAfterPost();

    check(dismissals == 1);
};

// A tooltip the app takes down itself: the press outside is the app's to act
// on, so it is neither a dismissal nor swallowed.
auto tOutsidePressIsKeptWhenAsked = test("Popup/outsidePressIsKeptWhenAsked") = []
{
    auto content = PressCounter {};
    auto owner = Window {content, ownerOptions()};
    orderInForRealPresses(owner);

    auto* ownerNative = nativeWindow(owner);

    auto options = popupOptions(owner);
    options.dismissOnOutsideClick = false;

    auto popup = Window {options};

    auto dismissals = 0;
    popup.events.onDismissRequested = [&dismissals] { ++dismissals; };

    auto inOwner = NSMakePoint(40.f, 40.f);
    [NSApp postEvent:pressAt(inOwner, ownerNative.windowNumber) atStart:YES];
    drainAfterPost();

    check(dismissals == 0);
    check(content.presses == 1);
};

// A submenu is part of the menu that opened it: its press is its own, and the
// menu above must neither take it nor go away over it. Swallowing here is what
// would make a submenu unclickable.
auto tSubmenuPressIsItsOwn = test("Popup/aSubmenusPressIsNotADismissal") = []
{
    auto owner = Window {ownerOptions()};
    orderInForRealPresses(owner);

    auto popup = Window {popupOptions(owner)};

    auto submenuContent = PressCounter {};
    auto submenu = Window {submenuContent, secondPopupOptions(popup)};
    orderInForRealPresses(submenu);

    auto dismissals = 0;
    popup.events.onDismissRequested = [&dismissals] { ++dismissals; };

    auto* submenuNative = nativeWindow(submenu);
    auto inSubmenu = NSMakePoint(20.f, 20.f);

    [NSApp postEvent:pressAt(inSubmenu, submenuNative.windowNumber) atStart:YES];
    drainAfterPost();

    check(dismissals == 0);
    check(submenuContent.presses == 1);
};

// A menu opened from somewhere else is a different menu: this one goes, and
// the press still belongs to the window it landed in — two popups that ate
// each other's clicks would leave both unusable.
auto tUnrelatedPopupPressDismissesOnly =
    test("Popup/anUnrelatedPopupsPressIsNotSwallowed") = []
{
    auto owner = Window {ownerOptions()};
    orderInForRealPresses(owner);

    auto first = Window {popupOptions(owner)};

    auto siblingContent = PressCounter {};
    auto sibling = Window {siblingContent, secondPopupOptions(owner)};
    orderInForRealPresses(sibling);

    auto dismissals = 0;
    first.events.onDismissRequested = [&dismissals] { ++dismissals; };

    auto* siblingNative = nativeWindow(sibling);
    auto inSibling = NSMakePoint(20.f, 20.f);

    [NSApp postEvent:pressAt(inSibling, siblingNative.windowNumber) atStart:YES];
    drainAfterPost();

    check(dismissals == 1);
    check(siblingContent.presses == 1);
};

// A menu is built and thrown away on every open, so the detach has to happen
// on the popup's own destruction — a child window left on the list keeps the
// owner holding it.
auto tDestroyingDetachesIt =
    test("Popup/destroyingItDetachesItFromTheOwner") = []
{
    auto owner = Window {ownerOptions()};

    {
        auto popup = Window {popupOptions(owner)};
        check(nativeWindow(owner).childWindows.count == 1);
    }

    check(nativeWindow(owner).childWindows.count == 0);
};

// And the other order, which is the one an app hits on quit: the owner is torn
// down with the menu still up.
auto tOwnerMayGoFirst = test("Popup/survivesTheOwnerGoingFirst") = []
{
    auto popup = std::unique_ptr<Window> {};

    {
        auto owner = Window {ownerOptions()};
        popup = std::make_unique<Window>(popupOptions(owner));

        check(nativeWindow(*popup).parentWindow != nil);
    }

    check(nativeWindow(*popup).parentWindow == nil);

    popup.reset();
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

    check(postAndWaitForDismissal(escapePress(), dismissals));
    check(popup == nullptr);
    check(nativeWindow(owner).childWindows.count == 0);
};

// Where a popup opened at a click goes. A borderless window is the case with
// no chrome in the arithmetic: its frame is its content, so the answer is the
// window's own top-left plus the view's offset inside it plus the point, and
// any of the three going missing shows up here.
auto tPlacementComesFromLocalToScreen =
    test("Popup/placementComesFromLocalToScreen") = []
{
    auto options = WindowOptions {};
    options.isPrimary = false;
    options.flags.clear();
    options.flags.add(WindowFlags::Borderless);
    options.width = 400;
    options.height = 300;
    options.initialPosition = eacp::Graphics::Point {300.f, 200.f};

    auto content = View {};
    auto child = View {};
    content.addSubview(child);

    auto window = Window {content, options};
    child.setBounds({30.f, 20.f, 100.f, 50.f});

    auto placement = child.localToScreen({5.f, 7.f});

    check(window.getPosition().x == 300.f);
    check(window.getPosition().y == 200.f);
    check(placement.x == 335.f);
    check(placement.y == 227.f);
};

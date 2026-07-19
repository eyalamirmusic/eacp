#import <AppKit/AppKit.h>

#include "Common.h"
#include <eacp/Graphics/Menu/Menu.h>

// The platform half of menu enablement. The model half is in MenuTests.cpp;
// what cannot be reached from there is whether AppKit is ever *told* about the
// predicate.
//
// Worth its own test because the failure is silent in the direction that costs
// something: a target that does not answer validateMenuItem: leaves every item
// enabled, so a menu of unavailable commands looks entirely normal and clicking
// one does nothing. Nothing else fails. That was the state of the framework
// before this method was registered — and NSObject does not implement
// validateMenuItem: itself, so unlike the scrollWheel: case, respondsToSelector:
// is a real signal here rather than one inheritance satisfies for free.

using namespace nano;
using namespace eacp::Graphics;

namespace
{
// The menu bar is installed on NSApp, so there has to be one. Apps::run creates
// it, but forcing it here keeps the test honest if that ever stops being true.
NSMenuItem* installAndFind(const MenuBar& bar, NSString* menuTitle, NSString* itemTitle)
{
    [NSApplication sharedApplication];

    // The menu bar is installed per window now, so the platforms that own menus
    // per window have one to attach to. macOS ignores it — the bar belongs to
    // the application — but the argument still has to be a real window.
    auto window = Window {};

    setApplicationMenuBar(bar, window);

    auto* mainMenu = [NSApp mainMenu];

    if (mainMenu == nil)
        return nil;

    for (NSMenuItem* container in mainMenu.itemArray)
        if ([container.submenu.title isEqualToString:menuTitle])
            return [container.submenu itemWithTitle:itemTitle];

    return nil;
}

MenuBar barWith(MenuItem item)
{
    auto menu = Menu {"Edit"};
    menu.add(std::move(item));

    auto bar = MenuBar {};
    bar.add(std::move(menu));

    return bar;
}
} // namespace

// The item's target answers validateMenuItem:, which is the one thing that has
// to be true for AppKit to consult the predicate at all. An NSMenu autoenables
// its items by default, and that default is what asks this question.
auto tTargetAnswersValidation = test("Menu/targetAnswersValidation") = []
{
    const auto bar = barWith(MenuItem::withAction("Undo"));

    auto* item = installAndFind(bar, @"Edit", @"Undo");

    check(item != nil);
    check(item.target != nil);
    check([item.target respondsToSelector:@selector(validateMenuItem:)]);
};

// A disabled command's item validates NO, which is what greys it.
auto tDisabledItemValidatesFalse = test("Menu/disabledItemValidatesFalse") = []
{
    const auto bar = barWith(
        MenuItem::withAction("Undo", [] {}, commandKey("z"), [] { return false; }));

    auto* item = installAndFind(bar, @"Edit", @"Undo");

    check(item != nil);
    check(![item.target validateMenuItem:item]);
};

auto tEnabledItemValidatesTrue = test("Menu/enabledItemValidatesTrue") = []
{
    const auto bar =
        barWith(MenuItem::withAction("Redo", [] {}, {}, [] { return true; }));

    auto* item = installAndFind(bar, @"Edit", @"Redo");

    check(item != nil);
    check([item.target validateMenuItem:item]);
};

// An item that said nothing about availability stays available. Every call site
// that predates enablement is in this case.
auto tUnspecifiedItemValidatesTrue = test("Menu/unspecifiedItemValidatesTrue") = []
{
    const auto bar = barWith(MenuItem::withAction("Paste"));

    auto* item = installAndFind(bar, @"Edit", @"Paste");

    check(item != nil);
    check([item.target validateMenuItem:item]);
};

// The predicate is asked each time rather than sampled when the bar was built.
// This is the property that lets an app install its menus once at startup:
// without it, greying would need a rebuild on every state change, during which
// AppKit may well be tracking the menu.
auto tValidationIsReadLive = test("Menu/validationIsReadLive") = []
{
    auto available = false;

    const auto bar = barWith(MenuItem::withAction("Save",
                                                  [] {},
                                                  commandKey("s"),
                                                  [&available] { return available; }));

    auto* item = installAndFind(bar, @"Edit", @"Save");

    check(item != nil);
    check(![item.target validateMenuItem:item]);

    available = true;

    // Same item, same target, no rebuild.
    check([item.target validateMenuItem:item]);
};

// A responder-selector item keeps its nil target, so validation goes down the
// responder chain to the focused view instead. Enablement must not have
// captured these — the focused view is the only thing that knows whether it can
// copy, and a C++ predicate here would override it.
auto tResponderItemKeepsNilTarget = test("Menu/responderItemKeepsNilTarget") = []
{
    const auto bar =
        barWith(MenuItem::withResponderSelector("Copy", "copy:", commandKey("c")));

    auto* item = installAndFind(bar, @"Edit", @"Copy");

    check(item != nil);
    check(item.target == nil);
    check(item.action == @selector(copy:));
};

// The action still runs. Adding a second method to the runtime class must not
// disturb the one that was already there.
auto tActionStillFires = test("Menu/actionStillFires") = []
{
    auto fired = false;

    const auto bar = barWith(MenuItem::withAction("Open", [&fired] { fired = true; }));

    auto* item = installAndFind(bar, @"Edit", @"Open");

    check(item != nil);

    [item.target performSelector:item.action withObject:item];

    check(fired);
};

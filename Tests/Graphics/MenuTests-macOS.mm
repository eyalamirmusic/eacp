#import <AppKit/AppKit.h>

#include "Common.h"
#include <eacp/Graphics/Menu/Menu.h>

using namespace nano;
using namespace eacp::Graphics;

namespace
{
NSMenuItem* installAndFind(const MenuBar& bar, NSString* menuTitle, NSString* itemTitle)
{
    [NSApplication sharedApplication];

    // macOS owns the bar per application, but setApplicationMenuBar still
    // needs a real window for the platforms that own menus per window.
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

auto tTargetAnswersValidation = test("Menu/targetAnswersValidation") = []
{
    const auto bar = barWith(MenuItem::withAction("Undo"));

    auto* item = installAndFind(bar, @"Edit", @"Undo");

    check(item != nil);
    check(item.target != nil);
    check([item.target respondsToSelector:@selector(validateMenuItem:)]);
};

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

auto tUnspecifiedItemValidatesTrue = test("Menu/unspecifiedItemValidatesTrue") = []
{
    const auto bar = barWith(MenuItem::withAction("Paste"));

    auto* item = installAndFind(bar, @"Edit", @"Paste");

    check(item != nil);
    check([item.target validateMenuItem:item]);
};

// Read live rather than sampled at build time, so an app can install its menus
// once at startup.
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

    check([item.target validateMenuItem:item]);
};

auto tValidationRefreshesTheCheckmark =
    test("Menu/validationRefreshesTheCheckmark") = []
{
    auto selected = false;

    const auto bar = barWith(MenuItem::withCheckableAction(
        "FaceTime HD Camera", [] {}, [&selected] { return selected; }));

    auto* item = installAndFind(bar, @"Edit", @"FaceTime HD Camera");

    check(item != nil);

    [item.target validateMenuItem:item];
    check(item.state == NSControlStateValueOff);

    selected = true;

    [item.target validateMenuItem:item];
    check(item.state == NSControlStateValueOn);
};

auto tUncheckableItemStateIsUntouched =
    test("Menu/uncheckableItemStateIsUntouched") = []
{
    const auto bar = barWith(MenuItem::withAction("Paste"));

    auto* item = installAndFind(bar, @"Edit", @"Paste");

    check(item != nil);

    item.state = NSControlStateValueOn;
    [item.target validateMenuItem:item];

    check(item.state == NSControlStateValueOn);
};

auto tResponderItemKeepsNilTarget = test("Menu/responderItemKeepsNilTarget") = []
{
    const auto bar =
        barWith(MenuItem::withResponderSelector("Copy", "copy:", commandKey("c")));

    auto* item = installAndFind(bar, @"Edit", @"Copy");

    check(item != nil);
    check(item.target == nil);
    check(item.action == @selector(copy:));
};

auto tActionStillFires = test("Menu/actionStillFires") = []
{
    auto fired = false;

    const auto bar = barWith(MenuItem::withAction("Open", [&fired] { fired = true; }));

    auto* item = installAndFind(bar, @"Edit", @"Open");

    check(item != nil);

    [item.target performSelector:item.action withObject:item];

    check(fired);
};

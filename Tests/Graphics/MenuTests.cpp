#include "Common.h"

// The portable half of menu enablement: the model that every backend reads.
// Whether an item actually greys out is an AppKit question and lives in
// MenuTests-macOS.mm; what is checkable everywhere is that the predicate
// survives construction and is never null, because the platform code calls it
// without asking.

using namespace nano;
using namespace eacp::Graphics;

// An item that says nothing about availability is available. This is the case
// every existing call site is in, so the default is what stops enablement from
// being a breaking change.
auto tDefaultItemIsEnabled = test("Menu/defaultItemIsEnabled") = []
{
    const auto item = MenuItem::withAction("Save");

    check(item.isEnabled != nullptr);
    check(item.isEnabled());
};

auto tPredicateIsKept = test("Menu/predicateIsKept") = []
{
    auto available = false;

    const auto item = MenuItem::withAction(
        "Undo", [] {}, commandKey("z"), [&available] { return available; });

    check(!item.isEnabled());

    // Read live rather than sampled at construction: the whole point is that
    // the app never rebuilds the bar to change what is greyed.
    available = true;
    check(item.isEnabled());
};

// A null predicate assigned directly to the field is replaced on the way in,
// so the platform code can call it unconditionally. withAction fills the same
// hole; this covers the aggregate-initialisation path that skips it.
auto tNullPredicateIsReplaced = test("Menu/nullPredicateIsReplaced") = []
{
    auto item = MenuItem {};
    item.title = "Paste";
    item.isEnabled = nullptr;

    auto menu = Menu {"Edit"};
    menu.add(std::move(item));

    check(menu.items.size() == 1);
    check(menu.items[0].isEnabled != nullptr);
    check(menu.items[0].isEnabled());
};

auto tNullActionIsReplaced = test("Menu/nullActionIsReplaced") = []
{
    auto item = MenuItem {};
    item.title = "Save";
    item.action = nullptr;

    auto menu = Menu {"File"};
    menu.add(std::move(item));

    check(menu.items[0].action != nullptr);

    // Calling it is the assertion: a null action here would terminate.
    menu.items[0].action();
};

// A responder-selector item defers to the focused view for both what it does
// and whether it can be done, so it carries no predicate of its own — but the
// field still has to be callable, since nothing downstream special-cases it.
auto tResponderItemHasCallablePredicate =
    test("Menu/responderItemHasCallablePredicate") = []
{
    const auto item =
        MenuItem::withResponderSelector("Copy", "copy:", commandKey("c"));

    check(item.responderSelector == "copy:");
    check(item.isEnabled != nullptr);
    check(item.isEnabled());
};

auto tSeparatorCarriesNoTitle = test("Menu/separatorCarriesNoTitle") = []
{
    const auto item = MenuItem::separator();

    check(item.isSeparator);
    check(item.title.empty());
    check(item.isEnabled != nullptr);
};

auto tSubmenuIsHeld = test("Menu/submenuIsHeld") = []
{
    auto file = Menu {"File"};
    file.add(MenuItem::withAction("Open"));

    const auto item = MenuItem::withSubmenu("File", std::move(file));

    check(item.submenu != nullptr);
    check(item.submenu->items.size() == 1);
    check(item.submenu->items[0].title == "Open");
};

auto tBarKeepsMenuOrder = test("Menu/barKeepsMenuOrder") = []
{
    auto bar = MenuBar {};
    bar.add(Menu {"File"});
    bar.add(Menu {"Edit"});

    check(bar.menus.size() == 2);
    check(bar.menus[0].title == "File");
    check(bar.menus[1].title == "Edit");
};

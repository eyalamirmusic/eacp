#include "Common.h"

using namespace nano;
using namespace eacp::Graphics;

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

    available = true;
    check(item.isEnabled());
};

// Covers the aggregate-initialisation path, which skips withAction's fill-in.
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

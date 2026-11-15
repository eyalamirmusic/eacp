#include <eacp/UI/UI.h>

#include <NanoTest/NanoTest.h>

#include <string>

// A Graphics::MenuBar drawn as a row of titles in the tree, each opening its
// menu as a PopupMenu: what setApplicationMenuBar shows on Linux. Nothing is
// rendered; what is checked is what each menu turned into when it opened,
// which one is open, and how the bar moves between them.

using namespace nano;
using namespace eacp;
using namespace eacp::UI;

namespace MenuBarComponentTests
{
namespace
{
namespace Menus = eacp::Graphics;

eacp::Graphics::MouseEvent mouseAt(Point position)
{
    auto event = eacp::Graphics::MouseEvent {};

    event.pos = position;
    event.downPos = position;

    return event;
}

KeyEvent keyOf(std::uint16_t code)
{
    auto event = KeyEvent {};
    event.keyCode = code;

    return event;
}

struct Harness
{
    Harness()
    {
        auto file = Menus::Menu {"File"};
        file.add(Menus::MenuItem::withAction(
            "Open", [this] { ++opened; }, Menus::commandKey("o")));
        file.addSeparator();
        file.add(Menus::MenuItem::withAction("Quit"));

        auto view = Menus::Menu {"View"};
        view.add(Menus::MenuItem::withCheckableAction(
            "Grid", [] {}, [this] { return gridShown; }));
        view.add(Menus::MenuItem::withAction(
            "Zoom", [] {}, {}, [this] { return canZoom; }));

        auto empty = Menus::Menu {"Empty"};

        auto help = Menus::Menu {"Help"};
        help.add(Menus::MenuItem::withAction("About"));

        auto menus = Menus::MenuBar {};
        menus.add(std::move(file));
        menus.add(std::move(view));
        menus.add(std::move(empty));
        menus.add(std::move(help));

        bar.setMenuBar(std::move(menus));
        bar.onOpenMenuChanged = [this](int index) { openChanges.add(index); };

        host.setAnimationClockEnabled(false);
        host.setBounds({0.f, 0.f, 600.f, 400.f});
        host.setRootComponent(root);

        root.setBounds({0.f, 0.f, 600.f, 400.f});
        root.addAndMakeVisible(bar);

        bar.setBounds({0.f, 0.f, 600.f, 28.f});
    }

    Point titleCentre(int index) const { return bar.getTitleBounds(index).center(); }

    void click(Point position)
    {
        host.mouseDown(mouseAt(position));
        host.mouseUp(mouseAt(position));
    }

    void move(Point position) { host.mouseMoved(mouseAt(position)); }

    void key(std::uint16_t code) { host.keyDown(keyOf(code)); }

    const PopupMenu& open() const { return bar.getOpenMenu(); }

    ComponentHost host;
    Component root;
    MenuBarComponent bar;

    bool gridShown = false;
    bool canZoom = true;
    int opened = 0;
    Vector<int> openChanges;
};
} // namespace

auto tTitlesInOrder = test("MenuBarComponent/titlesSitSideBySideInOrder") = []
{
    auto harness = Harness {};

    check(harness.bar.getNumMenus() == 4);

    auto first = harness.bar.getTitleBounds(0);
    auto second = harness.bar.getTitleBounds(1);

    check(first.w > 0.f && first.h == 28.f);
    check(second.x == first.right());
    check(harness.bar.getTitleAt(harness.titleCentre(1)) == 1);
    check(harness.bar.getTitleAt({599.f, 10.f}) == -1);
    check(harness.bar.getTitleBounds(4).isEmpty());
};

// Ticks and greying are read as the menu opens, never when the bar was built.
auto tStateIsReadAtOpen = test("MenuBarComponent/stateIsReadWhenTheMenuOpens") = []
{
    auto harness = Harness {};

    harness.click(harness.titleCentre(1));
    check(harness.bar.getOpenMenuIndex() == 1);
    check(!harness.open().getItem(0).ticked);
    check(harness.open().getItem(1).enabled);

    harness.bar.closeMenu();
    harness.gridShown = true;
    harness.canZoom = false;

    harness.click(harness.titleCentre(1));
    check(harness.open().getItem(0).ticked);
    check(!harness.open().getItem(1).enabled);
};

auto tConversion = test("MenuBarComponent/menusBecomePopupMenus") = []
{
    auto recent = Menus::Menu {"Recent"};
    recent.add(Menus::MenuItem::withAction("a.txt"));

    auto menu = Menus::Menu {"File"};
    menu.addSeparator();
    menu.add(Menus::MenuItem::withAction("Settings", [] {}, Menus::commandKey(",")));
    menu.addSeparator();
    menu.addSeparator();
    menu.add(Menus::MenuItem::withResponderSelector("Copy", "copy:"));
    menu.add(Menus::MenuItem::withSubmenu("Open Recent", std::move(recent)));
    menu.add(Menus::MenuItem::withSubmenu("Nothing", Menus::Menu {"Nothing"}));
    menu.addSeparator();

    auto popup = MenuBarComponent::toPopupMenu(menu);

    // No leading, doubled or trailing separator; no responder item, nothing
    // empty.
    check(popup.getNumItems() == 3);
    check(popup.getItem(0).text == "Settings");
    check(popup.getItem(0).shortcutText == "Ctrl+,");
    check(popup.getItem(1).isSeparator);
    check(popup.getItem(2).hasSubMenu());
    check(popup.getItem(2).subMenu->getItem(0).text == "a.txt");
};

auto tClickOpensAndCloses =
    test("MenuBarComponent/aClickOnATitleOpensAndCloses") = []
{
    auto harness = Harness {};

    harness.click(harness.titleCentre(0));

    check(harness.bar.getOpenMenuIndex() == 0);
    check(harness.open().isShowing());

    auto panel = harness.open().getPanelBounds();
    check(panel.y >= 28.f, "under the bar");
    check(panel.x == harness.bar.getTitleBounds(0).x, "under its title");

    // The open menu covers the tree, so the second click on the title is a
    // click outside the panel, which closes it.
    harness.click(harness.titleCentre(0));

    check(harness.bar.getOpenMenuIndex() == -1);
    check(!harness.open().isShowing());
    check(harness.openChanges.size() == 2);
    check(harness.openChanges[0] == 0 && harness.openChanges[1] == -1);
};

auto tChoosingRuns = test("MenuBarComponent/choosingAnItemRunsItAndCloses") = []
{
    auto harness = Harness {};

    harness.click(harness.titleCentre(0));
    harness.click(harness.open().getItemBounds(0, 0).center());

    check(harness.opened == 1);
    check(harness.bar.getOpenMenuIndex() == -1);
    check(harness.openChanges.size() == 2);
};

// Crossing to another title with a menu open opens that one, and moving
// between menus is not a close.
auto tHoverSwitches = test("MenuBarComponent/crossingATitleSwitchesMenus") = []
{
    auto harness = Harness {};

    harness.click(harness.titleCentre(0));
    harness.move(harness.titleCentre(1));

    check(harness.bar.getOpenMenuIndex() == 1);
    check(harness.open().getItem(0).text == "Grid");
    check(harness.openChanges.size() == 2);
    check(harness.openChanges[0] == 0 && harness.openChanges[1] == 1);

    // The empty menu has nothing to open; the one open stays.
    harness.move(harness.titleCentre(2));
    check(harness.bar.getOpenMenuIndex() == 1);

    // Off every title, nothing changes either.
    harness.move({590.f, 10.f});
    check(harness.bar.getOpenMenuIndex() == 1);
};

auto tArrowsStep = test("MenuBarComponent/leftAndRightStepAcrossMenus") = []
{
    auto harness = Harness {};

    harness.click(harness.titleCentre(1));

    // Past the empty menu to Help, then round to File.
    harness.key(KeyCode::RightArrow);
    check(harness.bar.getOpenMenuIndex() == 3);

    harness.key(KeyCode::RightArrow);
    check(harness.bar.getOpenMenuIndex() == 0);

    harness.key(KeyCode::LeftArrow);
    check(harness.bar.getOpenMenuIndex() == 3);

    harness.key(KeyCode::Escape);
    check(harness.bar.getOpenMenuIndex() == -1);
    check(harness.openChanges.size() == 5);
    check(harness.openChanges.back() == -1);
};

// An action that replaces the bar's menus destroys the menu it ran from.
auto tActionMayReplaceTheBar =
    test("MenuBarComponent/anActionMayReplaceTheMenus") = []
{
    auto harness = Harness {};
    auto menu = Menus::Menu {"Only"};

    menu.add(Menus::MenuItem::withAction("Replace",
                                         [&harness]
                                         {
                                             auto other = Menus::MenuBar {};
                                             auto single = Menus::Menu {"Other"};
                                             single.add(
                                                 Menus::MenuItem::withAction("x"));
                                             other.add(std::move(single));
                                             harness.bar.setMenuBar(other);
                                         }));

    auto menus = Menus::MenuBar {};
    menus.add(std::move(menu));
    harness.bar.setMenuBar(std::move(menus));

    harness.click(harness.titleCentre(0));
    harness.click(harness.open().getItemBounds(0, 0).center());

    check(harness.bar.getNumMenus() == 1);
    check(harness.bar.getOpenMenuIndex() == -1);
};
} // namespace MenuBarComponentTests

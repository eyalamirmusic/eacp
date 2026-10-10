#include <eacp/UI/UI.h>

#include <NanoTest/NanoTest.h>

#include <string>

// The menu a button or a right-click opens, drawn in the tree as ComboBox's
// list is: one component over the root, which is what puts it above the rest,
// makes a click anywhere else close it, and confines it to the tree's own
// surface -- so near an edge it opens the other way, and taller than the tree
// it scrolls.
//
// Nothing is rendered: what is checked is where the panels decided to be, what
// was highlighted, and what the menu answered.

using namespace nano;
using namespace eacp;
using namespace eacp::UI;

namespace
{
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

PopupMenu::Options optionsOfWidth(float width)
{
    auto options = PopupMenu::Options {};
    options.minimumWidth = width;

    return options;
}

struct Harness
{
    Harness()
    {
        host.setAnimationClockEnabled(false);
        host.setBounds({0.f, 0.f, 400.f, 300.f});
        host.setRootComponent(root);

        root.setBounds({0.f, 0.f, 400.f, 300.f});
        root.addAndMakeVisible(button);

        button.setBounds({20.f, 20.f, 100.f, 24.f});

        menu.addItem(1, "one");
        menu.addSeparator();
        menu.addItem(2, "two", false);
        menu.addItem(3, "three", true, true);

        menu.onResult = [this](int id)
        {
            lastResult = id;
            ++results;
        };
    }

    bool open(PopupMenu::Options options = optionsOfWidth(120.f))
    {
        return menu.showAt(button, button.getLocalBounds(), options);
    }

    void click(Point position)
    {
        host.mouseDown(mouseAt(position));
        host.mouseUp(mouseAt(position));
    }

    void move(Point position) { host.mouseMoved(mouseAt(position)); }

    void key(std::uint16_t code) { host.keyDown(keyOf(code)); }

    Point centreOf(int depth, int index) const
    {
        return menu.getItemBounds(depth, index).center();
    }

    ComponentHost host;
    Component root;
    Button button {"menu"};
    PopupMenu menu;

    int lastResult = -1;
    int results = 0;
};
} // namespace

auto tPopupOpensBelow = test("PopupMenu/opensUnderItsAnchorAboveTheTree") = []
{
    auto harness = Harness {};

    check(harness.open());
    check(harness.menu.isShowing());
    check(harness.root.getChildren().size() == 2, "one component, on the root");
    check(harness.root.getChildren().back() != &harness.button,
          "and last, so it is drawn and hit first");

    auto panel = harness.menu.getPanelBounds();

    check(panel.y >= harness.button.getBounds().bottom());
    check(panel.x == harness.button.getBounds().x);
    check(panel.w >= 120.f, "at least the width it was asked for");
};

auto tPopupOpensUpward = test("PopupMenu/opensOverItsAnchorNearTheBottom") = []
{
    auto harness = Harness {};

    harness.button.setBounds({20.f, 270.f, 100.f, 24.f});
    harness.open();

    auto panel = harness.menu.getPanelBounds();

    check(panel.bottom() <= harness.button.getBounds().y);
    check(panel.y >= 0.f);
};

auto tPopupPrefersAbove = test("PopupMenu/aboveIsTakenWhenAskedAndThereIsRoom") = []
{
    auto harness = Harness {};

    auto options = optionsOfWidth(120.f);
    options.placement = PopupMenu::Placement::Above;

    // No room above the button at the top, so it falls back to below.
    harness.open(options);
    check(harness.menu.getPanelBounds().y >= harness.button.getBounds().bottom());

    harness.menu.dismiss();
    harness.button.setBounds({20.f, 200.f, 100.f, 24.f});
    harness.open(options);
    check(harness.menu.getPanelBounds().bottom() <= harness.button.getBounds().y);
};

auto tPopupClampsToTheTree = test("PopupMenu/isKeptInsideTheTreeAtItsRightEdge") = []
{
    auto harness = Harness {};

    harness.button.setBounds({360.f, 20.f, 30.f, 24.f});
    harness.open();

    auto panel = harness.menu.getPanelBounds();

    check(panel.right() <= 400.f, "shifted left rather than cut off");
    check(panel.x >= 0.f);
};

auto tPopupAtMouse = test("PopupMenu/atTheMouseItsCornerIsThePress") = []
{
    auto harness = Harness {};

    auto event = MouseEvent {};
    event.position = {10.f, 5.f};

    check(harness.menu.showAtMouse(harness.button, event, optionsOfWidth(120.f)));

    auto panel = harness.menu.getPanelBounds();

    check(panel.x == 30.f && panel.y == 25.f);
};

auto tPopupRefusesOutsideATree = test("PopupMenu/refusesAComponentWithNoHost") = []
{
    auto loose = Component {};
    auto menu = PopupMenu {};

    menu.addItem(1, "one");

    check(!menu.showAt(loose, {0.f, 0.f, 10.f, 10.f}));
    check(!menu.isShowing());

    auto harness = Harness {};
    auto empty = PopupMenu {};

    check(!empty.showAt(harness.button, harness.button.getLocalBounds()));
};

auto tPopupClickChooses = test("PopupMenu/clickingAnItemAnswersWithItsId") = []
{
    auto harness = Harness {};
    auto ran = false;

    harness.menu.addItem("action", [&] { ran = true; });

    harness.open();
    harness.click(harness.centreOf(0, 3));

    check(!harness.menu.isShowing());
    check(harness.root.getChildren().size() == 1);
    check(harness.results == 1 && harness.lastResult == 3);

    harness.open();
    harness.click(harness.centreOf(0, 4));

    check(ran, "an action item runs its action");
    check(harness.results == 2 && harness.lastResult == 0);
};

auto tPopupDisabledIgnored =
    test("PopupMenu/clickingADisabledItemOrSeparatorDoesNothing") = []
{
    auto harness = Harness {};

    harness.open();
    harness.click(harness.centreOf(0, 2));
    harness.click(harness.centreOf(0, 1));

    check(harness.menu.isShowing());
    check(harness.results == 0);
};

auto tPopupOutsideDismisses = test("PopupMenu/aClickOutsideClosesItWithZero") = []
{
    auto harness = Harness {};
    auto clicked = 0;

    harness.button.onClick = [&] { ++clicked; };

    harness.open();
    harness.click({380.f, 280.f});

    check(!harness.menu.isShowing());
    check(harness.results == 1 && harness.lastResult == 0);

    harness.open();
    harness.click(harness.button.getBounds().center());

    check(!harness.menu.isShowing());
    check(clicked == 0, "the click that closes it is not passed on");
};

auto tPopupKeyboard = test("PopupMenu/keysSkipWhatCannotBeChosen") = []
{
    auto harness = Harness {};

    harness.open();

    check(harness.host.getFocusedComponent() != &harness.button);

    harness.key(KeyCode::DownArrow);
    check(harness.menu.getHighlightedIndex() == 0);

    harness.key(KeyCode::DownArrow);
    check(harness.menu.getHighlightedIndex() == 3,
          "past the separator and the disabled item");

    harness.key(KeyCode::DownArrow);
    check(harness.menu.getHighlightedIndex() == 3, "and not off the end");

    harness.key(KeyCode::UpArrow);
    check(harness.menu.getHighlightedIndex() == 0);

    harness.key(KeyCode::End);
    check(harness.menu.getHighlightedIndex() == 3);

    harness.key(KeyCode::Home);
    check(harness.menu.getHighlightedIndex() == 0);

    harness.key(KeyCode::Return);

    check(!harness.menu.isShowing());
    check(harness.lastResult == 1);
};

auto tPopupEscape = test("PopupMenu/escapeClosesItAndGivesFocusBack") = []
{
    auto harness = Harness {};
    auto editor = TextEditor {};

    harness.root.addAndMakeVisible(editor);
    editor.setBounds({200.f, 20.f, 100.f, 24.f});
    editor.grabKeyboardFocus();

    harness.open();
    check(harness.host.getFocusedComponent() != &editor);

    harness.key(KeyCode::Escape);

    check(!harness.menu.isShowing());
    check(harness.results == 1 && harness.lastResult == 0);
    check(harness.host.getFocusedComponent() == &editor);
};

auto tPopupHover = test("PopupMenu/hoverHighlightsWhatCanBeChosen") = []
{
    auto harness = Harness {};

    harness.open();

    harness.move(harness.centreOf(0, 3));
    check(harness.menu.getHighlightedIndex() == 3);

    harness.move(harness.centreOf(0, 2));
    check(harness.menu.getHighlightedIndex() == -1, "a disabled item is not lit");

    harness.move(harness.centreOf(0, 0));
    harness.move({380.f, 280.f});
    check(harness.menu.getHighlightedIndex() == -1);
};

namespace
{
struct SubMenuHarness : Harness
{
    SubMenuHarness()
    {
        auto sub = PopupMenu {};

        sub.addItem(10, "ten");
        sub.addSectionHeader("Header");
        sub.addItem(11, "eleven");

        menu.addSubMenu("more", std::move(sub));
    }
};
} // namespace

auto tPopupSubMenuByClick = test("PopupMenu/aSubMenuOpensBesideItsItem") = []
{
    auto harness = SubMenuHarness {};

    harness.open();
    harness.click(harness.centreOf(0, 4));

    check(harness.menu.isShowing(), "opening a submenu chooses nothing");
    check(harness.menu.getNumOpenPanels() == 2);

    auto parent = harness.menu.getPanelBounds(0);
    auto child = harness.menu.getPanelBounds(1);

    check(child.x >= parent.right() - 1.f, "to the right");
    check(std::abs(harness.menu.getItemBounds(1, 0).y
                   - harness.menu.getItemBounds(0, 4).y)
              < 0.5f,
          "its first row level with the row that opened it");

    harness.click(harness.centreOf(1, 2));

    check(!harness.menu.isShowing());
    check(harness.lastResult == 11, "answered through the menu that was shown");
};

auto tPopupSubMenuByHover = test("PopupMenu/aSubMenuOpensWhenThePointerRests") = []
{
    auto harness = SubMenuHarness {};

    harness.open();
    harness.move(harness.centreOf(0, 4));

    check(harness.menu.getNumOpenPanels() == 1, "not at once");

    harness.host.advanceAnimations(0.1);
    check(harness.menu.getNumOpenPanels() == 1);

    harness.host.advanceAnimations(0.15);
    check(harness.menu.getNumOpenPanels() == 2);

    // Crossing another row on the way into the submenu does not close it, so
    // long as the pointer gets there before the delay is up.
    harness.move(harness.centreOf(0, 3));
    harness.host.advanceAnimations(0.1);
    harness.move(harness.centreOf(1, 0));
    harness.host.advanceAnimations(0.5);

    check(harness.menu.getNumOpenPanels() == 2);
    check(harness.menu.getHighlightedIndex(0) == 4);
    check(harness.menu.getHighlightedIndex(1) == 0);

    // Resting on another row does.
    harness.move(harness.centreOf(0, 0));
    harness.host.advanceAnimations(0.5);

    check(harness.menu.getNumOpenPanels() == 1);
};

auto tPopupSubMenuByKeys = test("PopupMenu/rightOpensASubMenuAndLeftClosesIt") = []
{
    auto harness = SubMenuHarness {};

    harness.open();
    harness.key(KeyCode::End);
    harness.key(KeyCode::RightArrow);

    check(harness.menu.getNumOpenPanels() == 2);
    check(harness.menu.getHighlightedIndex(1) == 0);

    harness.key(KeyCode::DownArrow);
    check(harness.menu.getHighlightedIndex(1) == 2, "past the header");

    harness.key(KeyCode::LeftArrow);
    check(harness.menu.getNumOpenPanels() == 1);
    check(harness.menu.isShowing());

    harness.key(KeyCode::Return);
    check(harness.menu.getNumOpenPanels() == 2, "return opens it too");

    harness.key(KeyCode::Escape);
    check(harness.menu.getNumOpenPanels() == 1, "escape closes one level");

    harness.key(KeyCode::Escape);
    check(!harness.menu.isShowing());
    check(harness.lastResult == 0);
};

auto tPopupSubMenuOpensLeft =
    test("PopupMenu/aSubMenuWithNoRoomOnTheRightOpensLeft") = []
{
    auto harness = SubMenuHarness {};

    harness.button.setBounds({250.f, 20.f, 100.f, 24.f});
    harness.open();
    harness.click(harness.centreOf(0, 4));

    auto parent = harness.menu.getPanelBounds(0);
    auto child = harness.menu.getPanelBounds(1);

    check(child.right() <= parent.x + 1.f);
};

namespace
{
struct TallHarness : Harness
{
    TallHarness()
    {
        menu.clear();

        for (auto id = 1; id <= 40; ++id)
            menu.addItem(id, "item " + std::to_string(id));
    }
};
} // namespace

auto tPopupScrolls = test("PopupMenu/aMenuTallerThanTheTreeScrolls") = []
{
    auto harness = TallHarness {};

    harness.open();

    auto panel = harness.menu.getPanelBounds();

    check(panel.bottom() <= 300.f, "clipped to the tree");
    check(harness.menu.getScrollPosition() == 0.f);

    auto wheel = mouseAt(panel.center());
    wheel.delta = {0.f, -2.f};
    harness.host.mouseWheel(wheel);

    check(harness.menu.getScrollPosition() == 80.f, "two lines of wheel");

    harness.key(KeyCode::End);
    check(harness.menu.getHighlightedIndex() == 39);

    auto last = harness.menu.getItemBounds(0, 39);
    check(last.bottom() <= panel.bottom(), "the keyboard scrolls to what it lights");

    harness.click(last.center());
    check(harness.lastResult == 40);
};

auto tPopupDragScrolls = test("PopupMenu/draggingScrollsAndChoosesNothing") = []
{
    auto harness = TallHarness {};

    harness.open();

    auto start = harness.centreOf(0, 3);

    harness.host.mouseDown(mouseAt(start));
    harness.host.mouseDragged(mouseAt({start.x, start.y - 50.f}));
    harness.host.mouseUp(mouseAt({start.x, start.y - 50.f}));

    check(harness.menu.getScrollPosition() == 50.f);
    check(harness.menu.isShowing());
    check(harness.results == 0);
};

auto tPopupScrollArrow = test("PopupMenu/restingOnTheBottomArrowScrolls") = []
{
    auto harness = TallHarness {};

    harness.open();

    auto panel = harness.menu.getPanelBounds();
    auto style = PopupMenu::Style {};

    harness.move({panel.center().x,
                  panel.bottom() - style.padding - style.scrollArrowHeight * 0.5f});
    harness.host.advanceAnimations(0.0);
    harness.host.advanceAnimations(0.1);

    check(harness.menu.getScrollPosition() > 0.f);
};

auto tPopupStyle = test("PopupMenu/theStyleSetsTheRowHeight") = []
{
    auto harness = Harness {};

    auto options = optionsOfWidth(120.f);
    options.style.itemHeight = 40.f;
    options.style.separatorHeight = 10.f;
    options.style.padding = 6.f;

    harness.open(options);

    auto first = harness.menu.getItemBounds(0, 0);
    auto separator = harness.menu.getItemBounds(0, 1);

    check(first.h == 40.f && separator.h == 10.f);
    check(harness.menu.getPanelBounds().h == 40.f * 3.f + 10.f + 12.f);
    check(first.y == harness.menu.getPanelBounds().y + 6.f);
};

auto tPopupDestroyedWhileOpen =
    test("PopupMenu/destroyingAnOpenMenuTakesItsPanelsWithIt") = []
{
    auto harness = Harness {};
    auto results = 0;

    {
        auto menu = PopupMenu {};

        menu.addItem(1, "one");
        menu.onResult = [&](int) { ++results; };
        menu.showAt(harness.button, harness.button.getLocalBounds());

        check(harness.root.getChildren().size() == 2);
    }

    check(harness.root.getChildren().size() == 1);
    check(results == 0, "without a result");

    harness.click({40.f, 30.f});
    harness.move({40.f, 30.f});
};

auto tPopupResultMayReopen = test("PopupMenu/theResultMayShowTheMenuAgain") = []
{
    auto harness = Harness {};
    auto shown = 0;

    harness.menu.onResult = [&](int)
    {
        if (++shown == 1)
            harness.open();
    };

    harness.open();
    harness.click(harness.centreOf(0, 0));

    check(harness.menu.isShowing());
    check(shown == 1);
};

auto tPopupCopies = test("PopupMenu/aCopyHasItsOwnSubMenus") = []
{
    auto sub = PopupMenu {};
    sub.addItem(5, "five");

    auto menu = PopupMenu {};
    menu.addSubMenu("sub", sub);

    auto copy = menu;

    check(copy.getNumItems() == 1);
    check(copy.getItem(0).subMenu != nullptr);
    check(copy.getItem(0).subMenu.get() != menu.getItem(0).subMenu.get());
    check(copy.getItem(0).subMenu->getItem(0).id == 5);
};

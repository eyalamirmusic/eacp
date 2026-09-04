#include <eacp/UI/UI.h>

#include <NanoTest/NanoTest.h>

#include <string>

// The list a combo box opens, which is the part of it that is not a button.
//
// It is an ordinary component added to the root of the tree rather than a
// native menu, so everything a menu is normally given for free has to be true
// here on purpose: it is above the rest of the tree, a click anywhere else puts
// it away, and it opens upward when there is no room under the box -- which for
// a plugin editor is not a rare case but the bottom row of the window.
//
// Nothing is rendered: what is checked is where the list decided to be and what
// it did with the events aimed at it.

using namespace nano;
using namespace eacp;
using namespace eacp::UI;

namespace
{
// The box's own height, which is what a row of the list is; and the border the
// list draws around its rows.
constexpr auto boxHeight = 24.f;
constexpr auto listPadding = 4.f;

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
        host.setBounds({0.f, 0.f, 300.f, 200.f});
        host.setRootComponent(root);

        root.setBounds({0.f, 0.f, 300.f, 200.f});
        root.addAndMakeVisible(box);

        box.setBounds({20.f, 20.f, 120.f, boxHeight});
        box.addItems({"one", "two", "three", "four"});

        box.onChange = [this](int index)
        {
            lastChange = index;
            ++changes;
        };
    }

    void click(Point position)
    {
        host.mouseDown(mouseAt(position));
        host.mouseUp(mouseAt(position));
    }

    // The middle of a row of the open list, in the root's coordinates.
    Point rowCentre(int row) const
    {
        auto list = box.getPopupBounds();

        return {list.x + 20.f,
                list.y + listPadding + boxHeight * ((float) row + 0.5f)};
    }

    ComponentHost host;
    Component root;
    ComboBox box {"nothing"};

    int lastChange = -2;
    int changes = 0;
};
} // namespace

auto tComboSelectingIsSilent =
    test("ComboBox/settingTheSelectionReportsOnlyWhenAsked") = []
{
    auto harness = Harness {};

    harness.box.setSelectedIndex(2);

    check(harness.box.getSelectedIndex() == 2);
    check(harness.box.getText() == "three");
    check(harness.changes == 0);

    harness.box.setSelectedIndex(1, true);

    check(harness.changes == 1 && harness.lastChange == 1);

    // An index naming no item is refused rather than clamped, so a caller
    // cannot select something it did not name.
    harness.box.setSelectedIndex(9, true);

    check(harness.box.getSelectedIndex() == 1);
    check(harness.changes == 1);
};

auto tComboClickOpens = test("ComboBox/clickingTheBoxOpensTheList") = []
{
    auto harness = Harness {};

    check(!harness.box.isPopupOpen());
    check(harness.root.getChildren().size() == 1);

    harness.click({40.f, 30.f});

    check(harness.box.isPopupOpen());
    check(harness.root.getChildren().size() == 2,
          "the list is a child of the root, not of the box");
    check(!harness.box.getPopupBounds().isEmpty());
};

auto tComboClickingARowSelectsIt =
    test("ComboBox/clickingARowSelectsItAndCloses") = []
{
    auto harness = Harness {};

    harness.click({40.f, 30.f});
    harness.click(harness.rowCentre(2));

    check(harness.box.getSelectedIndex() == 2);
    check(harness.changes == 1 && harness.lastChange == 2, "and said so, once");
    check(!harness.box.isPopupOpen());
    check(harness.root.getChildren().size() == 1);
};

auto tComboClickingOutsideDismisses =
    test("ComboBox/aClickOutsideTheListPutsItAway") = []
{
    auto harness = Harness {};

    harness.box.setSelectedIndex(1);
    harness.click({40.f, 30.f});

    // Well clear of the list, which is 120 points wide under the box.
    harness.click({280.f, 190.f});

    check(!harness.box.isPopupOpen());
    check(harness.box.getSelectedIndex() == 1, "and changed nothing");
    check(harness.changes == 0);
};

auto tComboEscapeDismisses =
    test("ComboBox/escapeClosesTheListWithoutSelecting") = []
{
    auto harness = Harness {};

    harness.box.setSelectedIndex(0);
    harness.click({40.f, 30.f});

    check(harness.host.getFocusedComponent() == &harness.box,
          "opening takes the keyboard, or the keys below go elsewhere");

    harness.host.keyDown(keyOf(KeyCode::Escape));

    check(!harness.box.isPopupOpen());
    check(harness.box.getSelectedIndex() == 0);
    check(harness.changes == 0);
};

auto tComboArrowsAndReturn =
    test("ComboBox/arrowsMoveTheHighlightAndReturnTakesIt") = []
{
    auto harness = Harness {};

    harness.box.setSelectedIndex(1);
    harness.click({40.f, 30.f});

    // From the selection rather than from the top of the list.
    harness.host.keyDown(keyOf(KeyCode::DownArrow));
    harness.host.keyDown(keyOf(KeyCode::DownArrow));

    check(harness.box.getSelectedIndex() == 1, "a highlight is not a selection");
    check(harness.changes == 0);

    harness.host.keyDown(keyOf(KeyCode::Return));

    check(harness.box.getSelectedIndex() == 3);
    check(harness.changes == 1 && harness.lastChange == 3);
    check(!harness.box.isPopupOpen());
};

auto tComboArrowsWhileClosed =
    test("ComboBox/arrowsStepTheSelectionWhileClosed") = []
{
    auto harness = Harness {};

    harness.box.setSelectedIndex(1);
    harness.box.grabKeyboardFocus();

    harness.host.keyDown(keyOf(KeyCode::DownArrow));

    check(harness.box.getSelectedIndex() == 2);
    check(harness.changes == 1, "which is the user changing it, so it is reported");

    harness.host.keyDown(keyOf(KeyCode::UpArrow));
    harness.host.keyDown(keyOf(KeyCode::UpArrow));

    check(harness.box.getSelectedIndex() == 0);

    // Off the end does nothing rather than wrapping or deselecting.
    harness.host.keyDown(keyOf(KeyCode::UpArrow));

    check(harness.box.getSelectedIndex() == 0);
    check(harness.changes == 3);
};

// A plugin editor has no screen to spill onto: the tree's own surface is all
// the room there is, so a box near the bottom edge opens over itself.
auto tComboOpensUpward = test("ComboBox/aBoxNearTheBottomOpensUpward") = []
{
    auto harness = Harness {};

    harness.click({40.f, 30.f});

    check(harness.box.getPopupBounds().y >= harness.box.getBounds().bottom(),
          "with room below, the list is under the box");

    harness.click({280.f, 190.f});

    harness.box.setBounds({20.f, 170.f, 120.f, boxHeight});
    harness.click({40.f, 180.f});

    auto list = harness.box.getPopupBounds();

    check(harness.box.isPopupOpen());
    check(list.bottom() <= harness.box.getBounds().y, "and over it without");
    check(list.y >= 0.f, "and never off the top of the tree");
};

// A host outliving a component by a destructor or two is the normal case, and
// the list is a child of the root rather than of the box that made it.
auto tComboClosingTakesTheListWithIt =
    test("ComboBox/destroyingAnOpenBoxTakesItsListWithIt") = []
{
    auto host = ComponentHost {};
    auto root = Component {};

    host.setBounds({0.f, 0.f, 300.f, 200.f});
    host.setRootComponent(root);
    root.setBounds({0.f, 0.f, 300.f, 200.f});

    {
        auto box = ComboBox {};

        root.addAndMakeVisible(box);
        box.setBounds({20.f, 20.f, 120.f, boxHeight});
        box.addItems({"one", "two"});

        host.mouseDown(mouseAt({40.f, 30.f}));

        check(box.isPopupOpen());
        check(root.getChildren().size() == 2);
    }

    check(root.getChildren().size() == 0);

    // And the tree still answers rather than writing through what has gone.
    host.mouseDown(mouseAt({40.f, 30.f}));
    host.mouseUp(mouseAt({40.f, 30.f}));
    host.mouseMoved(mouseAt({40.f, 30.f}));
};

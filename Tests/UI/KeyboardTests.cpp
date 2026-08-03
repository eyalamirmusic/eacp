#include <eacp/UI/UI.h>

#include <NanoTest/NanoTest.h>

#include <string>

// Where a key goes, which is the only question the keyboard tier has to answer.
//
// A component tree lives in one native view, so the window hands every keystroke
// to the same object however deep the thing that should hear it is. What decides
// the rest is focus and the verdict a component returns -- and both are the kind
// of routing that looks obviously right and is wrong in one direction, so it is
// worth pinning down rather than trying by hand.
//
// No GPU here: the host is constructed but never rendered, and the events are
// synthesized. Focus, bubbling and traversal are all plain tree logic.

using namespace nano;
using namespace eacp;
using namespace eacp::UI;

namespace
{
// Records what it was given and consumes what it was told to.
struct Recorder final : Component
{
    explicit Recorder(std::string nameToUse, bool consumes = false)
        : name(std::move(nameToUse))
        , consuming(consumes)
    {
        setWantsKeyboardFocus(true);
        setInterceptsMouseClicks(true);
    }

    bool keyDown(const KeyEvent& event) override
    {
        keysSeen += event.characters;
        return consuming;
    }

    void focusGained() override
    {
        ++gained;

        // Asked from inside the callback on purpose: a component told it has
        // focus and then finding hasKeyboardFocus() false would be the kind of
        // half-applied state a caret blinks wrongly against.
        hadFocusWhenTold = hasKeyboardFocus();
    }

    void focusLost() override { ++lost; }

    std::string name;
    std::string keysSeen;
    bool consuming = false;
    int gained = 0;
    int lost = 0;
    bool hadFocusWhenTold = false;
};

KeyEvent keyOf(const std::string& characters, std::uint16_t code = 0)
{
    auto event = KeyEvent {};

    event.characters = characters;
    event.charactersIgnoringModifiers = characters;
    event.keyCode = code;

    return event;
}

KeyEvent tabKey(bool shift = false)
{
    auto event = keyOf("\t", eacp::Graphics::KeyCode::Tab);
    event.modifiers.shift = shift;

    return event;
}

eacp::Graphics::MouseEvent clickAt(Point position)
{
    auto event = eacp::Graphics::MouseEvent {};

    event.pos = position;
    event.downPos = position;

    return event;
}

// A host with a root and two focusable children, sized so a click can be aimed
// at either of them.
struct Harness
{
    Harness()
    {
        root.addAndMakeVisible(first);
        root.addAndMakeVisible(second);

        host.setBounds({0.f, 0.f, 200.f, 100.f});
        host.setRootComponent(root);

        root.setBounds({0.f, 0.f, 200.f, 100.f});
        first.setBounds({0.f, 0.f, 100.f, 100.f});
        second.setBounds({100.f, 0.f, 100.f, 100.f});
    }

    ComponentHost host;
    Recorder root {"root"};
    Recorder first {"first"};
    Recorder second {"second"};
};
} // namespace

auto tNothingFocusedGoesToTheRoot =
    test("Keyboard/withNothingFocusedKeysReachTheRoot") = []
{
    auto harness = Harness {};

    check(harness.host.getFocusedComponent() == nullptr);

    harness.host.keyDown(keyOf("a"));

    check(harness.root.keysSeen == "a", "so a shortcut works before any click");
    check(harness.first.keysSeen.empty());
};

auto tFocusedComponentGetsTheKey =
    test("Keyboard/theFocusedComponentIsOfferedFirst") = []
{
    auto harness = Harness {};

    harness.first.grabKeyboardFocus();
    harness.host.keyDown(keyOf("a"));

    check(harness.first.keysSeen == "a");
};

// The verdict is the whole mechanism: unconsumed, a key carries on up, which is
// what lets a root hold a shortcut while a field deep inside it has focus.
auto tUnconsumedKeysBubble = test("Keyboard/anUnconsumedKeyCarriesOnUpTheTree") = []
{
    auto harness = Harness {};

    harness.first.grabKeyboardFocus();
    harness.host.keyDown(keyOf("a"));

    check(harness.first.keysSeen == "a");
    check(harness.root.keysSeen == "a", "the parent heard it too");
};

auto tConsumedKeysStop = test("Keyboard/aConsumedKeyStopsWhereItWasTaken") = []
{
    auto harness = Harness {};

    harness.first.consuming = true;
    harness.first.grabKeyboardFocus();
    harness.host.keyDown(keyOf("a"));

    check(harness.first.keysSeen == "a");
    check(harness.root.keysSeen.empty(), "and the root never saw it");
};

auto tFocusCallbacks = test("Keyboard/focusIsHandedOverWithBothSidesTold") = []
{
    auto harness = Harness {};

    harness.first.grabKeyboardFocus();

    check(harness.first.gained == 1);
    check(harness.first.hadFocusWhenTold, "state applied before the callback");
    check(harness.first.lost == 0);

    harness.second.grabKeyboardFocus();

    check(harness.first.lost == 1);
    check(harness.second.gained == 1);
    check(harness.host.getFocusedComponent() == &harness.second);
};

auto tGrabbingTwiceIsNotAChange =
    test("Keyboard/regrabbingFocusTellsNobodyTwice") = []
{
    auto harness = Harness {};

    harness.first.grabKeyboardFocus();
    harness.first.grabKeyboardFocus();

    check(harness.first.gained == 1);
    check(harness.first.lost == 0);
};

auto tGivingAwayFocus = test("Keyboard/focusCanBeGivenBackToNothing") = []
{
    auto harness = Harness {};

    harness.first.grabKeyboardFocus();
    harness.first.giveAwayKeyboardFocus();

    check(harness.host.getFocusedComponent() == nullptr);
    check(harness.first.lost == 1);

    harness.host.keyDown(keyOf("a"));

    check(harness.root.keysSeen == "a");
    check(harness.first.keysSeen.empty());
};

// A component that has stopped wanting the keyboard and is still holding it
// swallows every key the tree sends, so it gives it up on the way out.
auto tRefusingFocusReleasesIt =
    test("Keyboard/aComponentThatStopsWantingFocusLosesIt") = []
{
    auto harness = Harness {};

    harness.first.grabKeyboardFocus();
    harness.first.setWantsKeyboardFocus(false);

    check(harness.host.getFocusedComponent() == nullptr);
    check(harness.first.lost == 1);
};

// Asking does not get around the flag, or setWantsKeyboardFocus(false) would
// mean two different things depending on which side of a grab it was called.
auto tGrabbingIsRefusedWhenUnwanted =
    test("Keyboard/aComponentThatWantsNoFocusCannotGrabIt") = []
{
    auto harness = Harness {};

    harness.second.setWantsKeyboardFocus(false);
    harness.second.grabKeyboardFocus();

    check(harness.host.getFocusedComponent() == nullptr);
    check(harness.second.gained == 0);
};

auto tPressMovesFocus = test("Keyboard/pressingSomethingFocusableFocusesIt") = []
{
    auto harness = Harness {};

    harness.host.mouseDown(clickAt({150.f, 50.f}));

    check(harness.host.getFocusedComponent() == &harness.second);
};

// A press with nothing focusable anywhere above it leaves focus where it was,
// rather than clearing it -- otherwise clicking a decorative panel silently
// disarms the field beside it.
//
// Note what this takes to arrange: the *root* has to want nothing either, since
// the walk below finds the nearest focusable ancestor and a focusable root is an
// ancestor of everything. Which is the honest description of the rule -- a tree
// whose root takes focus has no unfocusable clicks in it at all.
auto tPressOnUnfocusableKeepsFocus =
    test("Keyboard/pressingWithNothingFocusableUnderThePointerLeavesFocusAlone") = []
{
    auto harness = Harness {};

    harness.first.grabKeyboardFocus();
    harness.second.setWantsKeyboardFocus(false);
    harness.root.setWantsKeyboardFocus(false);

    harness.host.mouseDown(clickAt({150.f, 50.f}));

    check(harness.host.getFocusedComponent() == &harness.first);
};

// The nearest ancestor that wants it, not the deepest thing hit: a label inside
// a focusable row is what the pointer lands on and the row is what should end up
// holding the keyboard.
auto tPressFocusesTheNearestAncestor =
    test("Keyboard/pressingAChildFocusesTheAncestorThatWantsIt") = []
{
    auto harness = Harness {};

    auto child = Component {};
    child.setInterceptsMouseClicks(true);
    child.setBounds({0.f, 0.f, 50.f, 50.f});
    harness.second.addAndMakeVisible(child);

    harness.host.mouseDown(clickAt({120.f, 20.f}));

    check(harness.host.getFocusedComponent() == &harness.second);
};

auto tTabMovesFocus = test("Keyboard/tabMovesThroughTheTreeInAddOrder") = []
{
    auto harness = Harness {};

    harness.root.grabKeyboardFocus();

    harness.host.keyDown(tabKey());
    check(harness.host.getFocusedComponent() == &harness.first);

    harness.host.keyDown(tabKey());
    check(harness.host.getFocusedComponent() == &harness.second);

    // Wrapping, rather than trapping the keyboard at the end of the tree.
    harness.host.keyDown(tabKey());
    check(harness.host.getFocusedComponent() == &harness.root);
};

auto tShiftTabGoesBack = test("Keyboard/shiftTabMovesTheOtherWay") = []
{
    auto harness = Harness {};

    harness.second.grabKeyboardFocus();

    harness.host.keyDown(tabKey(true));

    check(harness.host.getFocusedComponent() == &harness.first);
};

// Traversal is a fallback and not a rule, so a component wanting Tab for itself
// takes it the same way it takes any other key.
auto tConsumedTabDoesNotTraverse = test("Keyboard/aConsumedTabDoesNotMoveFocus") = []
{
    auto harness = Harness {};

    harness.first.consuming = true;
    harness.first.grabKeyboardFocus();

    harness.host.keyDown(tabKey());

    check(harness.host.getFocusedComponent() == &harness.first);
    check(harness.first.keysSeen == "\t");
};

auto tTraversalCanBeTurnedOff = test("Keyboard/aTreeCanKeepTabForItself") = []
{
    auto harness = Harness {};

    harness.host.setTabMovesFocus(false);
    harness.first.grabKeyboardFocus();

    harness.host.keyDown(tabKey());

    check(harness.host.getFocusedComponent() == &harness.first);
};

// A host outliving its focused component by even one destructor is the normal
// case, so the pointer has to go with it.
auto tDeletedComponentLosesFocus =
    test("Keyboard/deletingTheFocusedComponentClearsIt") = []
{
    auto harness = Harness {};

    {
        auto temporary = Recorder {"temporary"};
        harness.root.addAndMakeVisible(temporary);
        temporary.grabKeyboardFocus();

        check(harness.host.getFocusedComponent() == &temporary);
    }

    check(harness.host.getFocusedComponent() == nullptr);

    // And the tree still works afterwards rather than writing through it.
    harness.host.keyDown(keyOf("a"));

    check(harness.root.keysSeen == "a");
};

// An invisible component is not a tab stop: focus landing on something that
// cannot be seen is a keyboard that has vanished as far as the user knows.
auto tHiddenComponentsAreSkipped = test("Keyboard/tabSkipsWhatIsNotVisible") = []
{
    auto harness = Harness {};

    harness.first.setVisible(false);
    harness.root.grabKeyboardFocus();

    harness.host.keyDown(tabKey());

    check(harness.host.getFocusedComponent() == &harness.second);
};

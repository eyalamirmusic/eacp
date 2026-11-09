#include "Common.h"

#include <eacp/Core/Utils/WinInclude.h>

// Windows has no responder chain, so the window used to hand every key to its
// content view and nothing else: a nested view that called focus() — a
// terminal pane, a ComponentHost overlay — never heard a key, where on macOS
// the same code worked because AppKit sends keys to the first responder. These
// drive real WM_KEYDOWN messages through the host window so the whole path is
// under test, not just View.

using namespace nano;
using namespace eacp;
using namespace eacp::Graphics;

namespace
{
// Records every key it is handed and passes on all but `keeps`.
struct KeyRecorder final : View
{
    void keyDown(const KeyEvent& event) override
    {
        events.add(event);

        if (event.keyCode != keeps)
            passKeyOn();
    }

    Vector<KeyEvent> events;
    uint16_t keeps = KeyCode::Unknown;
};

void pressKey(HWND hwnd, WPARAM virtualKey)
{
    SendMessageW(hwnd, WM_KEYDOWN, virtualKey, 0);
}

// The content view with a pane inside it and a leaf inside that: deep enough
// that the focused view is neither the root nor the root's direct child.
struct FocusTree
{
    FocusTree()
    {
        root.addSubview(pane);
        pane.addSubview(leaf);
        window.setContentView(root);
    }

    HWND hwnd() { return static_cast<HWND>(window.getHandle()); }

    KeyRecorder root;
    KeyRecorder pane;
    KeyRecorder leaf;
    Window window;
};
} // namespace

auto tKeysReachFocusedView = test("KeyFocus/keysReachTheFocusedNestedView") = []
{
    auto tree = FocusTree {};
    tree.leaf.keeps = KeyCode::A;
    tree.leaf.focus();

    pressKey(tree.hwnd(), 'A');

    check(tree.leaf.events.size() == 1, "the focused leaf got the key");
    check(tree.pane.events.empty(), "a kept key goes no further");
    check(tree.root.events.empty(), "the content view did not get it");
};

auto tPassedOnKeyBubbles = test("KeyFocus/aKeyPassedOnBubblesToEachParent") = []
{
    auto tree = FocusTree {};
    tree.leaf.keeps = KeyCode::A;
    tree.pane.keeps = KeyCode::B;
    tree.leaf.focus();

    pressKey(tree.hwnd(), 'B');
    pressKey(tree.hwnd(), 'C');

    check(tree.leaf.events.size() == 2, "the focused leaf is asked first");
    check(tree.pane.events.size() == 2, "its parent hears what it passed on");
    check(tree.root.events.size() == 1, "the pane kept B, so only C reaches root");

    if (tree.root.events.size() == 1)
        check(tree.root.events[0].keyCode == KeyCode::C);
};

auto tFocusMoves = test("KeyFocus/focusingAViewUnfocusesTheLast") = []
{
    auto tree = FocusTree {};
    tree.leaf.focus();
    tree.pane.focus();

    check(tree.pane.hasFocus());
    check(!tree.leaf.hasFocus(), "one view per window has key focus");

    pressKey(tree.hwnd(), 'A');

    check(tree.leaf.events.empty());
    check(tree.pane.events.size() == 1);
};

auto tNoFocusMeansContentView =
    test("KeyFocus/withNoFocusTheContentViewGetsKeys") = []
{
    auto tree = FocusTree {};

    pressKey(tree.hwnd(), 'A');

    check(tree.root.events.size() == 1);
    check(tree.pane.events.empty() && tree.leaf.events.empty());
};

auto tRemovedFocusFallsBack = test("KeyFocus/removingTheFocusedViewFallsBack") = []
{
    auto tree = FocusTree {};
    tree.leaf.focus();

    // Removing the pane takes the focused leaf out with it, and the window must
    // not go on routing keys to a view that is no longer in it.
    tree.root.removeSubview(tree.pane);

    check(!tree.leaf.hasFocus());

    pressKey(tree.hwnd(), 'A');

    check(tree.leaf.events.empty());
    check(tree.root.events.size() == 1, "keys go back to the content view");
};

// Ctrl+A's WM_CHAR is "\x01", which is what `characters` carries, but a
// shortcut matches the key it was pressed on. The thread's keyboard state is
// what the window reads that from, so a held Control is set there directly.
auto tIgnoringModifiers =
    test("KeyEvent/charactersIgnoringModifiersDropControl") = []
{
    auto tree = FocusTree {};
    tree.root.keeps = KeyCode::A;

    BYTE saved[256] = {};
    GetKeyboardState(saved);

    BYTE held[256] = {};
    GetKeyboardState(held);
    held[VK_CONTROL] = 0x80;
    held[VK_LCONTROL] = 0x80;
    SetKeyboardState(held);

    pressKey(tree.hwnd(), VK_CONTROL);
    PostMessageW(tree.hwnd(), WM_CHAR, 0x01, 0);
    pressKey(tree.hwnd(), 'A');

    SetKeyboardState(saved);

    const auto* event = tree.root.events.findIf([](const KeyEvent& e)
                                                { return e.keyCode == KeyCode::A; });
    check(event != nullptr);

    if (event == nullptr)
        return;

    check(event->modifiers.control);
    check(event->characters == "\x01", "characters is what the key typed");
    check(event->charactersIgnoringModifiers == "a", "and this, the key itself");
};
